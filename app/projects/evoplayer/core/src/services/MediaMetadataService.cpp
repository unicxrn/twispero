#include "evo/services/MediaMetadataService.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <cctype>
#include <algorithm>

namespace evo {

void MediaMetadataService::loadChapters(AVFormatContext* formatContext) {
    clearChapters();
    if (!formatContext || formatContext->nb_chapters <= 0) {
        return;
    }

    m_chapters.reserve(formatContext->nb_chapters);
    for (unsigned int i = 0; i < formatContext->nb_chapters && m_chapters.size() < 128; ++i) {
        AVChapter* chapter = formatContext->chapters[i];
        if (!chapter) continue;

        double startSec = chapter->start * av_q2d(chapter->time_base);
        if (startSec < 0.0) startSec = 0.0;

        ChapterInfo info;
        info.startTimeSeconds = startSec;

        const AVDictionaryEntry* titleEntry = av_dict_get(chapter->metadata, "title", nullptr, 0);
        if (titleEntry && titleEntry->value && titleEntry->value[0]) {
            info.title = titleEntry->value;
        } else {
            char fallback[48];
            std::snprintf(fallback, sizeof(fallback), "Chapter %zu", m_chapters.size() + 1);
            info.title = fallback;
        }

        m_chapters.push_back(std::move(info));
    }
}

void MediaMetadataService::clearChapters() {
    m_chapters.clear();
}

const ChapterInfo* MediaMetadataService::getChapter(size_t index) const {
    if (index < m_chapters.size()) {
        return &m_chapters[index];
    }
    return nullptr;
}

int MediaMetadataService::getChapterIndexAtPosition(double positionSeconds) const {
    int bestMatch = -1;
    for (size_t i = 0; i < m_chapters.size(); ++i) {
        if (m_chapters[i].startTimeSeconds <= positionSeconds + 0.05) {
            bestMatch = static_cast<int>(i);
        } else {
            break;
        }
    }
    return bestMatch;
}

void MediaMetadataService::cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) {
    if (filePath.empty()) {
        outTitle = "Unknown";
        outCategory = "Unknown";
        return;
    }

    size_t lastSlash = filePath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

    size_t lastDot = filename.find_last_of('.');
    std::string extension;
    if (lastDot != std::string::npos && lastDot > 0) {
        extension = filename.substr(lastDot + 1);
        filename = filename.substr(0, lastDot);
    }

    for (char& c : filename) {
        if (c == '.' || c == '_') {
            c = ' ';
        }
    }

    // Strip leading/trailing spaces
    size_t start = filename.find_first_not_of(" \t\r\n");
    size_t end = filename.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos) {
        filename = filename.substr(start, end - start + 1);
    }

    outTitle = filename;
    outCategory = extension.empty() ? "MEDIA" : extension;
    std::transform(outCategory.begin(), outCategory.end(), outCategory.begin(), ::toupper);
}

} // namespace evo
