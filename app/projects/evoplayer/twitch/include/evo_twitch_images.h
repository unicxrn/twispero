#pragma once
/*
 * evo_twitch_images.h - thumbnails, box art and avatars for the Twitch screens.
 *
 * A screen calls image() every frame for each picture it shows. The first call
 * queues a download; three worker threads fetch it with evo_net's sync GET,
 * decode it (JPEG or PNG, stb_image) and shrink it to fit maxW x maxH. Until
 * then image() returns an Image with rgba == nullptr and the screen draws a
 * skeleton in its place.
 *
 * Threads: image(), images_frame() and images_start() are main thread only.
 * The workers never touch the cache; they hand finished pixels back through a
 * mutex-protected list that images_frame() drains.
 *
 * Pixels: rows of R,G,B,A bytes, straight alpha, top row first, w * 4 bytes a
 * row. Read as 32-bit words that is EVO's 0xAABBGGRR, which is what
 * evo::kit::TextureCache::pixels() takes, so no conversion is needed.
 *
 * Lifetime: the service owns the pixels. They stay valid for at least 4 frames
 * after the last image() call that returned them (TextureCache reads them when
 * it uploads and hashes them on every pixels() call). Use them in the frame
 * you got them; call image() again next frame instead of keeping the pointer.
 *
 * Memory: up to 24 MiB of decoded pixels, least recently shown first out.
 * Pictures shown in the last 4 frames are never freed, so a screen that shows
 * more than 24 MiB at once goes over the budget until some leave the screen.
 *
 * Drawing one in a kit screen (cover_uv in evo_hui_twitch_common.cpp). Pass id
 * as the tag: TextureCache keys a texture by pointer, size, tag and a sparse
 * pixel hash, and the id changes whenever a buffer is freed and its address
 * reused for another picture, so a stale texture is never shown:
 *
 *   const evo_twitch::Image img = evo_twitch::image(card.thumbUrl.c_str(), 440, 248);
 *   const std::uint32_t tex = img.rgba ? ctx.textures.pixels(
 *       reinterpret_cast<const std::uint32_t *>(img.rgba), img.w, img.h, img.id) : 0;
 *   if (tex) list.image(tex, r, cover_uv(img.w, img.h, r.w, r.h), kWhite, 12.0f);
 *   else draw_skeleton(list, r);
 */

namespace evo_twitch {

struct Image {
  int w = 0, h = 0;
  const unsigned char* rgba = nullptr;  // w * h * 4 bytes, see the top of the file
  unsigned long long id = 0;            // unique per decoded buffer, never 0 when rgba is set
};

// Returns the decoded image when it is in memory; otherwise queues a download
// (one per url + size, however often it is asked for) and returns an Image
// with rgba == nullptr. Call every frame for what is on screen: that is what
// keeps a picture in memory and a queued download from being dropped.
// maxW/maxH: decode-time downscale target (keeps the aspect ratio, never
// upscales); 0 = no limit on that side. A download that failed is not retried
// for 60 s. Main thread only.
Image image(const char* url, int maxW, int maxH) noexcept;

// The same picture when it is in memory, without queuing anything otherwise
// (a download in flight or a failure both give rgba == nullptr). For showing
// the picture a card had before a refresh gave it a new URL, while the new
// one downloads: an old URL is never fetched again. Keeps a picture it returns
// in memory like image(). Main thread only.
Image image_if_ready(const char* url, int maxW, int maxH) noexcept;

// Spawns the three worker threads. A later call starts only the workers that
// failed to start before (pthread_create refused), so calling it again is
// harmless. Call after evo_net is up; downloads asked for before then wait in
// the queue, which holds 64 (see image()). Main thread only.
void images_start() noexcept;

// Once per frame on the main thread, before the screens draw: publishes
// finished downloads (a few per frame, so a burst does not upload a dozen
// textures in one frame), frees pictures over the budget and forgets old
// failures. Cheap when there is nothing to do.
void images_frame() noexcept;

}  // namespace evo_twitch
