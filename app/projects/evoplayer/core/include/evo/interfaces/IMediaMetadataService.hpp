#ifndef EVO_I_MEDIA_METADATA_SERVICE_HPP
#define EVO_I_MEDIA_METADATA_SERVICE_HPP

#include "evo/Common.hpp"
#include <string>
#include <vector>

struct AVFormatContext;

namespace evo {

struct ChapterInfo {
    double startTimeSeconds = 0.0;
    std::string title;
};

/**
 * @brief Interface for a stream's chapters and the title derived from a path.
 */
class IMediaMetadataService {
public:
    virtual ~IMediaMetadataService() = default;

    virtual void loadChapters(AVFormatContext* formatContext) = 0;
    virtual void clearChapters() = 0;
    virtual size_t getChapterCount() const = 0;
    virtual const ChapterInfo* getChapter(size_t index) const = 0;
    virtual int getChapterIndexAtPosition(double positionSeconds) const = 0;

    virtual void cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) = 0;
};

} // namespace evo

#endif // EVO_I_MEDIA_METADATA_SERVICE_HPP
