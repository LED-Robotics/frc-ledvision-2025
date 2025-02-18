
#include <iostream>
#include <vector>
#include <cameraserver/CameraServer.h>
#include <thread>
#include <chrono>
#include <apriltag/frc/apriltag/AprilTagDetector.h>
#include <apriltag/frc/apriltag/AprilTagDetector_cv.h>
#include <apriltag/frc/apriltag/AprilTagPoseEstimator.h>
#include "common.hpp"

using namespace frc;

class Camera {
  public:
    Camera(cs::UsbCamera *cam, cs::VideoMode config, AprilTagPoseEstimator::Config estConfig);

    // AprilTag Detection struct
    struct TagDetection {
      uint8_t id = -1;
      std::vector<AprilTagDetection::Point> corners;
      Transform3d transform;
    };

    enum MLMode {
      Detect,
      Pose
    };

    // Fetch Camera id
    uint8_t GetID();
    
    // Get current AprilTags being estimated
    std::vector<uint8_t> GetTargetTags();

    // Set current AprilTags being estimated
    void SetTargetTags(std::vector<uint8_t> targets);

    // Get current TagDetection vector from Camera
    std::vector<Camera::TagDetection>* GetTagDetections();

    // Get total current tag detections
    int GetTagDetectionCount();

    // Get current box ML Detection vector from Camera
    std::vector<det::DetectObject>* GetBoxDetections();

    // Get current box ML Detection vector from Camera
    std::vector<det::PoseObject>* GetPoseDetections();

    // Get total current ML detections
    int GetMLDetectionCount();

    // Get current ML detection mode
    int GetMLDetectionMode();

    // Set current ML detection mode
    void SetMLDetectionMode(int mode);

    // Get system time (millis) of last frame grab
    uint32_t GetCaptureTime();

    // Stop overwriting the tag detection buffer
    void PauseTagDetection();

    // Resume overwriting the tag detection buffer
    void ResumeTagDetection();

    // Draw AprilTag outline on frame
    void DrawAprilTagBox(cv::Mat frame, TagDetection* tag);

    // Draw ML detection on frame
    void DrawDetectBox(cv::Mat frame, std::vector<det::DetectObject> &detections);

    // Draw ML detection on frame
    void DrawPoseBox(cv::Mat frame, std::vector<det::PoseObject> &detections);

    // Check if ML frame is ready
    bool IsMLFrameAvailable();

    // Set ML frame for refresh
    void SetMLFrameUnavailable();

    // Return ML frame for inference
    cv::Mat GetMLFrame();

    // Check if there is currently a valid frame from the Camera
    bool ValidPresent();

    // Start all threads except machine learning
    void StartStream();

    // Start frame collector
    void StartCollector();

    // Start frame cloning/transformation
    void StartGrayscaleConverter();

    // Start processing frames
    void StartProcessor();
    
    // Start labelling frames
    void StartLabeller();

    // Start posting labelled frames
    void StartPosting();
  
  private:
    const int threadDelay = 1;
    std::vector<uint8_t> targetTags{22, 18};

    uint8_t id = -1;
    cs::UsbCamera *cam = nullptr;
    cs::CvSink *sink = nullptr;
    cs::CvSource *source = nullptr;
    AprilTagDetector detector{};
    AprilTagPoseEstimator estimator;
    cv::Mat frame{};
    cv::Mat mlFrame{};
    cv::Mat gray{};
    cv::Mat labelled{};
    bool mlEnabled = true;
  
    uint32_t captureTime = 0;
    unsigned long lastFail = 0;
    bool newFrame = false;
    bool newInference = false;
    bool frameProcessed = false;
    bool validFrame = false;
    bool grayAvailable = false;
    bool frameLabelled = false;
    bool framePosted = false;
    bool mlFrameAvailable = false;
    bool newDetections = false;
    bool pauseTagDetections = false;

    std::vector<TagDetection> tagDetections;
    int tagDetectionCount = 0;
    int mlDetectionCount = 0;
    int mlMode = MLMode::Detect;
    std::vector<det::DetectObject> boxDetections;
    std::vector<det::PoseObject> poseDetections;

    std::thread collector;
    std::thread converter;
    std::thread processor;
    std::thread labeller;
    std::thread poster;
};
