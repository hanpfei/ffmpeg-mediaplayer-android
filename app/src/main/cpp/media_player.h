#pragma once

#include <memory>
#include <string>

class MediaPlayer {
public:
    std::shared_ptr<MediaPlayer> create(const std::string &url);
public:
    MediaPlayer();
    ~MediaPlayer();
    void setDataSource(const std::string &url);

private:
    std::string url_;
};
