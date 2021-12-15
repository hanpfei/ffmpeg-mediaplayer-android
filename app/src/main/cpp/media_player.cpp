
#include "media_player.h"

std::shared_ptr<MediaPlayer> MediaPlayer::create(const std::string &url) {
    auto media_player = std::make_shared<MediaPlayer>();
    media_player->setDataSource(url);
    return media_player;
}

MediaPlayer::MediaPlayer() {
}

MediaPlayer::~MediaPlayer() {
}

void MediaPlayer::setDataSource(const std::string &url) {
    url_ = url;
}