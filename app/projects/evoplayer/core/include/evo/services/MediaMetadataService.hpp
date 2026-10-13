#ifndef EVO_MEDIA_METADATA_SERVICE_HPP
#define EVO_MEDIA_METADATA_SERVICE_HPP

#include "evo/interfaces/IMediaMetadataService.hpp"
#include <vector>

namespace evo {

class MediaMetadataService : public IMediaMetadataService {
public:
    MediaMetadataService() = default;
    ~MediaMetadataService() override = default;

    void loadChapters(AVFormatContext* formatContext) override;
    void clearChapters() override;
    size_t getChapterCount() const override { return m_chapters.size(); }
    const ChapterInfo* getChapter(size_t index) const override;
    int getChapterIndexAtPosition(double positionSeconds) const override;

    void cleanMediaTitle(const std::string& filePath, std::string& outTitle, std::string& outCategory) override;

private:
    std::vector<ChapterInfo> m_chapters;
};

} // namespace evo

#endif // EVO_MEDIA_METADATA_SERVICE_HPP
