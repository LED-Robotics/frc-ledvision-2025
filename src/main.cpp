#include <iostream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>
#include <networktables/NetworkTableInstance.h>
#include <networktables/NetworkTable.h>
#include <apriltag/frc/apriltag/AprilTagDetector.h>
#include <apriltag/frc/apriltag/AprilTagDetector_cv.h>
#include <apriltag/frc/apriltag/AprilTagPoseEstimator.h>
#include <apriltag/frc/apriltag/AprilTagFieldLayout.h>
#include <apriltag/frc/apriltag/AprilTagFields.h>
#include <cameraserver/CameraServer.h>
#include <units/length.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <linux/in.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp> 

#include <nlohmann/json.hpp>
using json = nlohmann::json;
using namespace frc;

// Camera resolution/format configs
int width = 640;
int height = 640;
cs::VideoMode camConfig{cs::VideoMode::PixelFormat::kMJPEG, width, height, 30};

struct Camera {
  int id = -1;
  cs::UsbCamera* ref = nullptr;
  cs::CvSink* sink = nullptr;
  cs::CvSource* source = nullptr;
  cv::Mat frame{};
  cv::Mat gray{};
  bool validData = false;
};

struct Detection {
    int label = -1;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::vector<float> kps = {};
};

int inferTarget = -1;
cv::Mat inferFrame;
json detectionJson;
std::vector<struct Detection> detections = {};
bool newInference = false;
bool newFrame = false;
std::vector<std::string> classes = {"label", "x", "y", "width", "height"};

// AprilTag detection objects
AprilTagDetector detector{};
AprilTagPoseEstimator estimator{{140_mm, (double)width, (double)height, (double)width/2, (double)height/2}};  // dummy numbers

// Signature for Jetson comms
const uchar udpSignature[] = {0x5b, 0x20, 0xc4, 0x10};

// Max datagram length for image stream
const int MaxDatagram = 32768;
uchar request[MaxDatagram];
uchar response[32768];

int getSocket(struct sockaddr_in *server_addr) {
    int sock = -1;
    int yes = 1;
    std::cout << "Server address is " << inet_ntoa(server_addr->sin_addr) << ':' << htons(server_addr->sin_port) << std::endl;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cout << "sock error" << std::endl;
    }
    int ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (ret == -1) {
        perror("setsockopt error");
        return 0;
    }
    return sock;
}

// Find IP Address and Port of Jetson YOLOv8 runner
int getMLServer(struct sockaddr_in *server_address) {
    const uchar discoverSignature[] = {0x8e, 0x96};
    uchar request[6];
    memcpy(&request, udpSignature, 4);
    memcpy(&request[4], discoverSignature, 2);
    int sock;
    int yes = 1;
    struct sockaddr_in broadcast_addr;
    struct sockaddr_in server_addr;
    socklen_t addr_len;
    int count;
    int ret;
    fd_set readfd;
    char buffer[100];
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("sock error");
        return -1;
    }
    ret = setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&yes, sizeof(yes));
    if (ret == -1) {
        perror("setsockopt error");
        return 0;
    }

    addr_len = sizeof(struct sockaddr_in);

    memset((void*)&broadcast_addr, 0, addr_len);
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    broadcast_addr.sin_port = htons(5555);

    ret = sendto(sock, request, sizeof(request), 0, (struct sockaddr*) &broadcast_addr, addr_len);

    FD_ZERO(&readfd);
    FD_SET(sock, &readfd);
    ret = select(sock + 1, &readfd, NULL, NULL, NULL);
    while(memcmp(buffer, request, sizeof(request))) {
        if (ret > 0) {
            if (FD_ISSET(sock, &readfd)) {
                count = recvfrom(sock, buffer, 1024, 0, (struct sockaddr*)&server_addr, &addr_len);
                    server_address->sin_family = server_addr.sin_family;
                    server_address->sin_addr = server_addr.sin_addr;
                    server_address->sin_port = server_addr.sin_port;
            }
        }
    }

    return 1;
}

// Send datagram to provided address using provided socket
int sendReceive(int sock, struct sockaddr_in *server_addr, uchar* req_buf, int reqSize, uchar* buf, int bufSize) {
    socklen_t addr_len;
    int count;
    int ret;
    fd_set readfd;
    addr_len = sizeof(struct sockaddr_in);
    struct timeval timeout;
    timeout.tv_usec = 500000;

    ret = sendto(sock, req_buf, reqSize, 0, (struct sockaddr*) server_addr, addr_len);
    FD_ZERO(&readfd);
    FD_SET(sock, &readfd);
    ret = select(sock + 1, &readfd, NULL, NULL, &timeout);
    if (ret > 0) {
        if (FD_ISSET(sock, &readfd)) {
            count = recvfrom(sock, buf, bufSize, 0, (struct sockaddr*) server_addr, &addr_len);
        }
    }
    return 1;
}
// Request inferencing on a frame
json remoteInference(int sock, struct sockaddr_in *server_addr, cv::Mat frame) {
    // Create message header buffer
    const uchar inferSignature[] = {0xe2, 0x4d};
    uchar header[sizeof(udpSignature) + sizeof(inferSignature)];
    memcpy(&header[0], udpSignature, sizeof(udpSignature));
    memcpy(&header[sizeof(udpSignature)], inferSignature, sizeof(inferSignature));
    memcpy(request, header, sizeof(header));


    // Chunk our frame into manageable pieces 
    const int MaxChunk = MaxDatagram - sizeof(header) - 1;  // extra config byte after header
    std::vector<uchar> frameVec;
    cv::imencode(".jpg", frame, frameVec);
    // CHUNK CHUNK CHUNK CHUNK
    const int vectorSize = frameVec.size();
    uchar* rawVector = frameVec.data();
    int totalChunks = ceil((double)vectorSize / (double)MaxChunk);
    for(int i = 0; i < totalChunks; i++) {
        int offset = (i * MaxChunk);
        bool lastChunk = offset + MaxChunk >= vectorSize;
        int size = lastChunk ? vectorSize - offset : MaxChunk;
        memcpy(request + sizeof(header) + 1, rawVector + offset, size);
        request[sizeof(header)] = lastChunk;
        sendReceive(sock, server_addr, request, sizeof(header) + 1 + size, response, sizeof(response));
    }

    if(!memcmp(header, response, sizeof(header))) {
        
        uchar sizeHigh = response[sizeof(header)];
        uchar sizeLow = response[sizeof(header) + 1];
        unsigned int size = (sizeHigh << 8) + sizeLow;
        if(size && size < sizeof(response)) {   // Valid JSON is present
            std::string jsonString(reinterpret_cast<char*>(response + sizeof(header) + 2), size);
            // std::cout << jsonString << std::endl;
            json detections = json::parse(jsonString);
            return detections;
        }
    }
    return {};
}

void initAprilTagDetector() {
    // Configure AprilTag detector
    detector.AddFamily("tag36h11");
    detector.SetConfig({});
    auto quadParams = detector.GetQuadThresholdParameters();
    quadParams.minClusterPixels = 3;
    detector.SetQuadThresholdParameters(quadParams);
}

void debugTagPrint(int id, Transform3d transform) {
    std::cout << "Tag " << id << " Pose Estimation:" << std::endl;
    std::cout << "X Off: " << units::foot_t{transform.X()}.value();
    std::cout << " Y Off: " << units::foot_t{transform.Y()}.value();
    std::cout << " Z Off: " << units::foot_t{transform.Z()}.value() << std::endl;
    std::cout << "Rot Off: " << transform.Rotation().ToRotation2d().Degrees().value() << std::endl;
    std::cout << std::endl;
}

// Init and return all cameras plugged in
std::vector<cs::UsbCamera> initCameras(cs::VideoMode config) {
    CS_Status status = 0;
    std::vector<cs::UsbCamera> cameras{};
    for (const auto& caminfo : cs::EnumerateUsbCameras(&status)) {
        fmt::print("Dev {}: Path {} (Name {})\n", caminfo.dev, caminfo.path, caminfo.name);
        fmt::print("vid {}: pid {}\n", caminfo.vendorId, caminfo.productId);
        cs::UsbCamera cam{"camera-" + caminfo.dev, caminfo.path};
        cam.SetVideoMode(config);
        cameras.push_back(cam);
    }
    return cameras;
}

void drawAprilTagBox(cv::Mat frame, const frc::AprilTagDetection* tag) {
  // Draw boxes around tags for video feed                
  for(int i = 0; i < 4; i++) {
      auto point1 = tag->GetCorner(i);
      int secondIndex = i == 3 ? 0 : i + 1;   // out of bounds adjust for last iteration
      auto point2 = tag->GetCorner(secondIndex);
      cv::Point lineStart{(int)point1.x, (int)point1.y};
      cv::Point lineEnd{(int)point2.x, (int)point2.y};
      cv::line(frame, lineStart, lineEnd, cv::Scalar(0, 0, 255), 2, cv::LINE_4);
  }
}

void constructDetections() {
  detections.clear();
  for (auto& detection : detectionJson) {
  // std::cout << detection << std::endl;
      for(auto& jsonClass : classes) {
          if(!detection.contains(jsonClass))
              continue;
      }
      int label = detection["label"];
      if(label != 0) continue;    // only box notes
      double x = detection["x"];
      double y = detection["y"];
      double width = detection["width"];
      double height = detection["height"];
      if(detection.contains("kps")) {
        std::vector<float> kps;
        auto points = detection["kps"];
        for(auto& point : points) {
          kps.push_back(point["x"]);
          kps.push_back(point["y"]);
          kps.push_back(point["s"]);
          detections.push_back({label, x, y, width, height, kps});
        }
      } else {
        detections.push_back({label, x, y, width, height});
      }
  }
  // std::cout << std::endl;
  newInference = false;
}

void drawInferenceBox(cv::Mat frame) {
  for (auto& detection : detections) {
      cv::Rect rect(detection.x, detection.y, detection.width, detection.height);
      cv::rectangle(frame, rect, cv::Scalar(255, 0, 0), 2, cv::LINE_4);
      for(int i = 0; i < detection.kps.size(); i += 3) {
        cv::Point center(detection.kps[i], detection.kps[i+1]);
        cv::circle(frame, center, detection.kps[i+2]*4, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_8);
      }
  }
}

int main(int argc, char** argv)
{  
    // Initialize AprilTag detector
    initAprilTagDetector();
    // Initialize cameras
    auto rawCameras = initCameras(camConfig);
    std::vector<Camera> cameras;
    for(cs::UsbCamera& cam : rawCameras) {
        auto info = cam.GetInfo();
        std::cout << "Camera found: " << std::endl;
        std::cout << info.path << ", " << info.name << std::endl;
        cameras.push_back(Camera{info.dev, &cam});
    }

    // Construct camera sink/sources
    for(Camera& cam : cameras) {
      if(cam.ref == nullptr) continue;
      std::cout << "Cam ID: " << cam.id << std::endl;
      cam.sink = new cs::CvSink{frc::CameraServer::GetVideo(*cam.ref)};
      cam.source = new cs::CvSource{"source" + cam.id, camConfig};
    }

    if(!cameras.size()) {
      std::cout << "No viable cameras found!" << std::endl;
      return 0;
    }

    inferTarget = cameras[0].id;

    // Start capture on CvSources
    // TCP ports start at 1181 
    for(Camera& cam : cameras) {
      frc::CameraServer::StartAutomaticCapture(*cam.source);
    }
    
    // NT Initialization
    auto inst = nt::NetworkTableInstance::GetDefault();
    inst.SetServerTeam(6722);
    inst.StartClient4("jetson-client");
    auto table = inst.GetTable("/jetson");
    
    // Spin up separate thread to request inferencing
    std::thread inferThread([&]{
        // Find Jetson IP and port
        struct sockaddr_in server_addr;
        int result = getMLServer(&server_addr);
        // Configure socket for inference
        int sock = getSocket(&server_addr);
        while(true) {
            // Do remote inference on frame
            if(!newInference && newFrame) {
                newFrame = false;
                if(inferFrame.empty()) continue;
                detectionJson = remoteInference(sock, &server_addr, inferFrame);
                // detectionJson is a global, so data is allowed to be stale
                // this is so inference doesn't block AprilTag processing
                newInference = true;
            }
        }
    });

    while(true) {
      // Collect frames from cameras
      // Done separately to try and keep cameras synced in real-time
      for(Camera& cam : cameras) {
        int success = cam.sink->GrabFrame(cam.frame);
        cam.validData = !cam.frame.empty();
        if (cam.validData) {
          cv::cvtColor(cam.frame, cam.gray, cv::COLOR_BGR2GRAY);
          
          // Clone target camera frame into inference buffer
          if(cam.id == inferTarget) {
            if(!newFrame) {
                inferFrame = cam.frame.clone();
            }
            newFrame = true;
          }
        }
      }

      // Fill detections array if new detections have been sent
      // This being up-to-date is the inferThread's responsibility
      if(newInference) {
        constructDetections();
      }

      // Main AprilTag processing loop. Done once per camera
      for(Camera& cam : cameras) {
        if(!cam.validData) continue;
        
        // Draw detections onto frame
        if(cam.id == inferTarget) {
          drawInferenceBox(cam.frame);
        }
        
        auto detections = frc::AprilTagDetect(detector, cam.gray);

        for(const frc::AprilTagDetection* tag : detections) {
          auto transform = estimator.Estimate(*tag);  // Estimate Transform3d relative to camera
          // Print relative offset
          debugTagPrint(tag->GetId(), transform);
          
          // Draw box on our frame
          drawAprilTagBox(cam.frame, tag); 
        }
      }

      // Write frames to publishing source
      // Done separately because synced web streams are nice
      for(Camera& cam : cameras) {
        if(cam.validData) cam.source->PutFrame(cam.frame);
      }
    }
}
