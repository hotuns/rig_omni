#ifndef CAMERA_H
#define CAMERA_H

#include <string>
#include <stdexcept>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual void ReleaseFrame() {}  // 归还帧缓冲，默认空实现
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;
    virtual std::string UploadSnapshot(const std::string& url) {
        throw std::runtime_error("Camera snapshot upload is not supported");
    }
};

#endif // CAMERA_H
