#pragma once

void extractAudio(const char *srcPath, const char *dstPath);

int decodeAudio(const char *src_file_path, const char *out_file_path);

void extractVideo(const char *srcPath, const char *dstPath);
int decodeVideo(const char *srcPath, const char *dstPath);

int extractAndDecodeAudio(const char *srcPath, const char *dstPath, const char *dstResampledPath);
