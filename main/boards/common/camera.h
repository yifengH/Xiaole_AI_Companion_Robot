#ifndef CAMERA_H
#define CAMERA_H

#include <stdexcept>
#include <string>
#include <vector>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool CaptureJpeg(std::vector<uint8_t>& out, int quality = 70) { return false; }
    virtual std::string CaptureAndExplain(const std::string& question) {
        if (!Capture()) {
            throw std::runtime_error("Failed to capture photo");
        }
        return Explain(question);
    }
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual bool SetSwapBytes(bool enabled) { return false; }  // Optional, default no-op
    virtual std::string Explain(const std::string& question) = 0;
};

#endif // CAMERA_H
