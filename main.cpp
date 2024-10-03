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

// AprilTag detection objects
AprilTagDetector detector{};
AprilTagPoseEstimator estimator{{140_mm, (double)width, (double)height, (double)width/2, (double)height/2}};  // dummy numbers
auto tagLayout = AprilTagFieldLayout::LoadField(AprilTagField::k2024Crescendo);

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
    timeout.tv_usec = 50000;

    ret = sendto(sock, req_buf, reqSize, 0, (struct sockaddr*) server_addr, addr_len);
    FD_ZERO(&readfd);
    FD_SET(sock, &readfd);
    ret = select(sock + 1, &readfd, NULL, NULL, NULL);
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
            std::string jsonString(reinterpret_cast<char*>(response + sizeof(header) + 2), size) ;
            // std::cout << jsonString << std::endl;
            json detections = json::parse(jsonString);
            return detections;
        }
    }
    return {};
}

void printCoordinates(int id) {
    auto idSearch = tagLayout.GetTagPose(id);
    if(idSearch.has_value()) {  // ensure tag ID exists in our field coordinate system
        auto tag = idSearch.value();
        auto tagTranslation = tag.Translation();
        std::cout << "Tag " << id << " Pose:" << std::endl;
        std::cout << "X: " << tagTranslation.X().value() << "Y: " << tagTranslation.Y().value()<< "Z: " << tagTranslation.Z().value() << std::endl;
        std::cout << "Rotation: " << tag.Rotation().ToRotation2d().Degrees().value() << std::endl;
    } else {
        std::cout << "Tag with id " << id << " was not found" << std::endl;
    }
}

Pose3d GetTagPose(int id) {
    auto idSearch = tagLayout.GetTagPose(id);
    if(idSearch.has_value()) {  // ensure tag ID exists in our field coordinate system
        auto tagPose = idSearch.value();
        return tagPose;
    } else {
        std::cout << "Tag with id " << id << " was not found" << std::endl;
        return {};
    }
}

// Init and return all cameras plugged in
std::vector<cs::UsbCamera> initCameras(cs::VideoMode config) {
    CS_Status status = 0;
    std::vector<cs::UsbCamera> cameras{};
    int number = 0;
    for (const auto& caminfo : cs::EnumerateUsbCameras(&status)) {
        fmt::print("Dev {}: Path {} (Name {})\n", caminfo.dev, caminfo.path, caminfo.name);
        fmt::print("vid {}: pid {}\n", caminfo.vendorId, caminfo.productId);
        cs::UsbCamera cam{"camera-" + number, caminfo.path};
        cam.SetVideoMode(config);
        cameras.push_back(cam);
    }
    return cameras;
}

int main(int argc, char** argv)
{   
    // Find Jetson IP and port
    struct sockaddr_in server_addr;
    int result = getMLServer(&server_addr);
    // Configure socket for inference
    int sock = getSocket(&server_addr);
    // Configure AprilTag detector
    detector.AddFamily("tag36h11");
    detector.SetConfig({});
    auto quadParams = detector.GetQuadThresholdParameters();
    quadParams.minClusterPixels = 3;
    detector.SetQuadThresholdParameters(quadParams);
    tagLayout.SetOrigin(AprilTagFieldLayout::OriginPosition::kBlueAllianceWallRightSide);
    // Initialize cameras
    auto cameras = initCameras(camConfig);
    cs::UsbCamera* testCam = nullptr;
    for(cs::UsbCamera& cam : cameras) {
        auto info = cam.GetInfo();
        std::cout << "Camera found: " << std::endl;
        std::cout << info.path << ", " << info.name << std::endl;
        if(info.name == "Razer Kiyo Pro Ultra") {
            std::cout << "Test cam found!" << std::endl;
            testCam = &cam;
        }
    }
    // Make sure camera was found and initialized
    if(testCam != nullptr) {
        // Setup camera stream
        cs::CvSink testSink{frc::CameraServer::GetVideo(*testCam)};
        cs::CvSource testSource{"testSource", camConfig};
        frc::CameraServer::StartAutomaticCapture(testSource);
        cv::Mat frame, grayFrame;

        while(true) {
            int frameTime = testSink.GrabFrameNoTimeout(frame);

            // Do remote inference on frame
            auto mlDetections = remoteInference(sock, &server_addr, frame);
            for (auto& detection : mlDetections) {
                // std::cout << detection << std::endl;
                int label = detection["label"];
                if(label != 0) continue;    // only box notes
                double x = detection["x"];
                double y = detection["y"];
                double width = detection["width"];
                double height = detection["height"];
                cv::Rect rect(x, y, width, height);
                cv::rectangle(frame, rect, cv::Scalar(255, 0, 0), 2, cv::LINE_4);
            }

            // AprilTag detection requires grayscale
            cv::cvtColor(frame, grayFrame, cv::COLOR_BGR2GRAY);
            
            auto detections = frc::AprilTagDetect(detector, grayFrame);
            // std::cout << detections.size() << " detections!" << std::endl;
            for(const frc::AprilTagDetection* tag : detections) {
                auto transform = estimator.Estimate(*tag);  // Estimate Transform3d relative to camera
                // Print relative offset
                std::cout << "Tag " << tag->GetId() << " Pose Estimation:" << std::endl;
                std::cout << "X Off: " << units::foot_t{transform.X()}.value();
                std::cout << " Y Off: " << units::foot_t{transform.Y()}.value();
                std::cout << " Z Off: " << units::foot_t{transform.Z()}.value() << std::endl;
                std::cout << "Rot Off: " << transform.Rotation().ToRotation2d().Degrees().value() << std::endl;
                std::cout << std::endl;
                // Print estimated field-coordinates of the camera using the tag detection
                auto realPose = GetTagPose(tag->GetId());   // actual field coordinate of tag
                auto estimatedPose = realPose + transform;  // wow this is so easy
                auto estTranslation = estimatedPose.Translation();
                std::cout << "Estimated Pose on Field: " << std::endl;
                std::cout << "X: " << estTranslation.X().value() << "Y: " << estTranslation.Y().value()<< "Z: " << estTranslation.Z().value() << std::endl;
                std::cout << "Rotation: " << estimatedPose.Rotation().ToRotation2d().Degrees().value() << std::endl;
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

            testSource.PutFrame(frame); // post to stream
        }
    }
}
