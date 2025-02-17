#include "Camera.h"

Camera::Camera(cs::UsbCamera *camRef, cs::VideoMode config, AprilTagPoseEstimator::Config estConfig) 
  : estimator{estConfig} {
  cam = camRef;
  // Configure AprilTag detector
  detector.AddFamily("tag36h11");
  detector.SetConfig({});
  auto quadParams = detector.GetQuadThresholdParameters();
  quadParams.minClusterPixels = 3;
  detector.SetQuadThresholdParameters(quadParams);

  auto info = cam->GetInfo();
  id = info.dev;
  sink = new cs::CvSink{frc::CameraServer::GetVideo(*cam)};
  source = new cs::CvSource{"source" + id, config};
  cam->SetVideoMode(config);
  frc::CameraServer::StartAutomaticCapture(*source);
}

uint8_t Camera::GetID() {
  return id;
}

std::vector<uint8_t> Camera::GetTargetTags() {
  return targetTags;
}

void Camera::SetTargetTags(std::vector<uint8_t> targets) {
  targetTags = targets;
}

std::vector<Camera::TagDetection>* Camera::GetTagDetections() {
  return &tagDetections;
}

int Camera::GetTagDetectionCount() {
  return tagDetectionCount;
}

std::vector<PeripherySession::Detection>* Camera::GetMLDetections() {
  return &mlDetections;
}

int Camera::GetMLDetectionCount() {
  return mlDetectionCount;
}

uint32_t Camera::GetCaptureTime() {
  return captureTime;
}

// Stop overwriting the tag detection buffer
void Camera::PauseTagDetection() {
  pauseTagDetections = true;
}

// Resume overwriting the tag detection buffer
void Camera::ResumeTagDetection() {
  pauseTagDetections = false;
}

// Draw AprilTag outline onto provided frame
void Camera::DrawAprilTagBox(cv::Mat frame, TagDetection* tag) {
  // Draw boxes around tags for video feed                
  for(int i = 0; i < 4; i++) {
      auto point1 = tag->corners[i];
      int secondIndex = i == 3 ? 0 : i + 1;   // out of bounds adjust for last iteration
      auto point2 = tag->corners[secondIndex];
      cv::Point lineStart{(int)point1.x, (int)point1.y};
      cv::Point lineEnd{(int)point2.x, (int)point2.y};
      cv::line(frame, lineStart, lineEnd, cv::Scalar(0, 0, 255), 2, cv::LINE_4);
  }
}

// Draw ML inference outlines onto provided frame
void Camera::DrawInferenceBox(cv::Mat frame, std::vector<PeripherySession::Detection> &detections) {
  for (auto& detection : detections) {
    cv::Rect rect(detection.x, detection.y, detection.width, detection.height);
    auto color = cv::Scalar((detection.label == 0) * 255, (detection.label == 1) * 255, (detection.label == 2) * 255);
    cv::rectangle(frame, rect, color, 2, cv::LINE_4);
    for(int i = 0; i < detection.kps.size(); i += 3) {
      cv::Point center(detection.kps[i], detection.kps[i+1]);
      cv::circle(frame, center, detection.kps[i+2]*4, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_8);
    }
  }
}

bool Camera::ValidPresent() {
  return newFrame && validFrame;
}

void Camera::StartStream() {
  std::cout << "Starting Capture for Cam " << (int)id << std::endl;
  collector = std::move(std::thread(&Camera::StartCollector, this));
  converter = std::move(std::thread(&Camera::StartGrayscaleConverter, this));
  processor = std::move(std::thread(&Camera::StartProcessor, this));
  labeller = std::move(std::thread(&Camera::StartLabeller, this));
  poster = std::move(std::thread(&Camera::StartPosting, this));
}

void Camera::StartCollector() {
  while(true) {
    if(newFrame && !framePosted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(threadDelay));
      continue;
    }
    // Get the current time from the system clock
    auto now = std::chrono::system_clock::now();

    // Convert the current time to time since epoch
    auto duration = now.time_since_epoch();
    unsigned long milliseconds
      = std::chrono::duration_cast<std::chrono::milliseconds>(
      duration).count();
      
    if(lastFail && milliseconds - lastFail > 3000) {
      continue;
    }
    auto success = sink->GrabFrame(frame);
    if(success == 0) {
      lastFail = milliseconds;
    } else {
      lastFail = 0;
    }
    validFrame = !lastFail && !frame.empty();
    if(validFrame) {
      captureTime = milliseconds + success;
      newFrame = true;
      grayAvailable = false;
      frameLabelled = false;
      frameProcessed = false;
      framePosted = false;
    }
  }
}

void Camera::StartGrayscaleConverter() {
  while(true) {
    if(!ValidPresent()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(threadDelay));
      continue;
    }
    if(!grayAvailable) {
      cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
      if(!mlFrameAvailable) {
        mlFrame = frame.clone();
        mlFrameAvailable = true;
      }
      labelled = frame.clone();
      grayAvailable = true;
    }
  }
}

void Camera::StartProcessor() {
  while(true) {
    if(!ValidPresent() || !grayAvailable) {
      std::this_thread::sleep_for(std::chrono::milliseconds(threadDelay));
      continue;
    }
    if(!frameProcessed) {
      if(!pauseTagDetections) {
        tagDetections.clear();
      }
      auto aprilTags = frc::AprilTagDetect(detector, gray);
      for(const frc::AprilTagDetection* tag : aprilTags) {
        uint8_t id = tag->GetId();
        uint8_t found = count(targetTags.begin(), targetTags.end(), id);
        if(!found) continue;  // tag not in request array, skip
        auto transform = estimator.Estimate(*tag);  // Estimate Transform3d of tag
        std::vector<AprilTagDetection::Point> corners;
        // Generate rectangle for labelling tag 
        for(int i = 0; i < 4; i++) {
            auto point = tag->GetCorner(i);
            corners.push_back(point);
        }
        if(!pauseTagDetections) {
          TagDetection data{id, corners, transform};
          tagDetections.push_back(data);
        }
      }
      tagDetectionCount = tagDetections.size();
      frameProcessed = true;
    }
  }
}

void Camera::StopInferencing() {
  if(mlSessions.size()) {
    mlSessionAvailable = false;
    mlThread.join();
    mlSessions.clear();
  }
}

void Camera::StartInferencing(PeripherySession session) {
  mlSessions.push_back(session);
  mlSessionAvailable = true;
  mlThread = std::move(std::thread(&Camera::InferenceThread, this));
}

void Camera::InferenceThread() {
  while(true) {
    if(!mlSessionAvailable) {
      return;
    }
    if(!mlFrameAvailable && !newDetections) {
      std::this_thread::sleep_for(std::chrono::milliseconds(0));
      continue;
    }
    auto detections = mlSessions[0].RunInference(mlFrame);
    mlDetections = detections;
    mlDetectionCount = mlDetections.size();
    mlFrameAvailable = false;
  }
}

void Camera::StartLabeller() {
  while(true) {
    if(!ValidPresent() || !frameProcessed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(threadDelay));
      continue;
    }
    for(TagDetection& tag : tagDetections) {
      DrawAprilTagBox(labelled, &tag);
    }
    DrawInferenceBox(labelled, mlDetections);
    frameLabelled = true;
  }
}

void Camera::StartPosting() {
  while(true) {
    if(!ValidPresent() || !frameLabelled) {
      std::this_thread::sleep_for(std::chrono::milliseconds(threadDelay));
      continue;
    }
    source->PutFrame(labelled);
    newFrame = false;
    frameProcessed = true;
  }
}

bool Camera::GetMLSessionAvailable() {
  return mlSessionAvailable;
}

uint32_t Camera::GetMLSessionID() {
  if(mlSessionAvailable) return mlSessions[0].GetID();
  else return 0;
}
