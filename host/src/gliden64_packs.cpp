// GLideN64 texture packs (issue #63): a texture cache (.htc) in the mods folder (put there, or installed
// from the Mods menu, whose installer copies it in) is unpacked into a texture pack folder beside it (the
// .htc's name without the extension): the textures as PNGs named for Rice
// (<name>#<crc>#<format>#<size>[#<palette crc>]_all.png), an rt64.json whose auto path is Rice, and a
// mod.json. RT64 (rt64.patch, hle/rt64_rice_hash.cpp) works out each texture's Rice hash as the game
// loads it and uses the pack's file for it, so the pack needs no dumped database. It's then a pack like
// any other, in the Mods menu and the Texture Packs settings.
//
// Unpacking is done when the launcher opens and when the Mods menu rescans the mods folder, on a thread
// of its own while the main thread shows the progress in a notification (a large pack takes a minute or
// so), then has the runtime open the packs again (turning a new one on, as any new mod). It's done once:
// gliden64_pack.json in the folder records the .htc's size and time, and the folder is unpacked again
// only when they change. The folder is about the .htc's size again.
//
// The .htc format (GLideN64, src/GLideNHQ/TxCache.cpp, TxMemoryCache): a gzip stream of an int32
// version (0x08000000; older caches, without the textures' N64 format and size, start with the config
// instead and aren't unpacked), an int32 config, then per texture: uint64 checksum (low word the
// texture's Rice CRC, high word its palette's), uint32 width and height, uint32 GL format (bit 31: the
// data is zlib-compressed), uint16 GL texture format, uint16 GL pixel type, uint8 is_hires, uint16 N64
// format (low byte) and size (high byte), uint32 data size, then the data. Only RGBA8 textures are
// unpacked (caches built with texture compression store S3TC). tools/texture_packs/gliden64_to_rt64.py
// converts the same packs to a database-based pack instead, from textures dumped while playing.
//
// GLideN64's texture storage (.hts, src/GLideNHQ/TxCache.cpp, TxFileStorage) holds the same textures,
// read from the file as they're needed instead of all at once: not gzipped. An int32 version (the same;
// older storages, again without the N64 format and size, start with the config and aren't unpacked), an
// int32 config, an int64 offset of the index; the textures as the .htc's, without their checksums (from
// width to the data); then, at the offset, the index: an int32 count, and per texture its uint64
// checksum and an int64 whose low 48 bits are its offset. Packs are made in it now too (GameBeast92's).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Before librecomp/mods.hpp, which includes miniz without its compression (MINIZ_NO_DEFLATE_APIS); the
// library has it.
#include "miniz.h"
#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "recompui/recompui.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    constexpr int32_t htc_version = 0x08000000;
    constexpr uint32_t gl_texfmt_gz = 0x80000000;
    constexpr uint16_t gl_unsigned_byte = 0x1401;
    const char* const source_record = "gliden64_pack.json";
    const char* const rom_name = "CONKER BFD";

    // Reads a gzip file's (first member's) data, with miniz's raw inflate.
    class GzipReader {
    public:
        explicit GzipReader(const std::filesystem::path& path) : file(path, std::ios::binary), in(1 << 20) {
            std::memset(&stream, 0, sizeof(stream));
            uint8_t header[10];
            if (!file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != 0x1F || header[1] != 0x8B || header[2] != 8) {
                return;
            }
            const uint8_t flags = header[3];
            if (flags & 0x04) { // FEXTRA
                uint8_t length[2];
                file.read(reinterpret_cast<char*>(length), 2);
                file.ignore(length[0] | (length[1] << 8));
            }
            for (uint8_t zero_terminated : { uint8_t(0x08), uint8_t(0x10) }) { // FNAME, FCOMMENT
                if (flags & zero_terminated) {
                    char c;
                    while (file.get(c) && c != 0) {
                    }
                }
            }
            if (flags & 0x02) { // FHCRC
                file.ignore(2);
            }
            ok = file.good() && mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
            initialized = ok;
        }

        ~GzipReader() {
            if (initialized) {
                mz_inflateEnd(&stream);
            }
        }

        // False at the end of the data, or on an error (see failed()).
        bool read(void* destination, size_t size) {
            if (!ok) {
                return false;
            }
            stream.next_out = static_cast<unsigned char*>(destination);
            stream.avail_out = (unsigned int)size;
            while (stream.avail_out > 0) {
                if (ended) {
                    return false;
                }
                if (stream.avail_in == 0) {
                    file.read(reinterpret_cast<char*>(in.data()), in.size());
                    stream.next_in = in.data();
                    stream.avail_in = (unsigned int)file.gcount();
                    file_bytes_read += stream.avail_in;
                    if (stream.avail_in == 0) {
                        ok = false;
                        return false;
                    }
                }
                const int status = mz_inflate(&stream, MZ_NO_FLUSH);
                if (status == MZ_STREAM_END) {
                    ended = true;
                }
                else if (status != MZ_OK) {
                    ok = false;
                    return false;
                }
            }
            return true;
        }

        template <typename T>
        bool read_value(T& value) {
            return read(&value, sizeof(value));
        }

        bool failed() const {
            return !ok;
        }

        // How much of the (compressed) file has been read, for the progress.
        uint64_t bytes_read() const {
            return file_bytes_read;
        }

    private:
        uint64_t file_bytes_read = 0;
        std::ifstream file;
        std::vector<uint8_t> in;
        mz_stream stream;
        bool ok = false;
        bool initialized = false;
        bool ended = false;
    };

    bool write_png(const std::filesystem::path& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
        const size_t stride = size_t(width) * 4;
        std::vector<uint8_t> raw;
        raw.reserve((stride + 1) * height);
        for (uint32_t y = 0; y < height; y++) {
            raw.push_back(0);
            raw.insert(raw.end(), rgba + y * stride, rgba + (y + 1) * stride);
        }
        mz_ulong compressed_size = mz_compressBound((mz_ulong)raw.size());
        std::vector<uint8_t> compressed(compressed_size);
        if (mz_compress2(compressed.data(), &compressed_size, raw.data(), (mz_ulong)raw.size(), MZ_BEST_SPEED) != MZ_OK) {
            return false;
        }
        compressed.resize(compressed_size);

        std::ofstream file(path, std::ios::binary);
        auto put_u32 = [&](uint32_t value) {
            const uint8_t bytes[4] = { uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value) };
            file.write(reinterpret_cast<const char*>(bytes), 4);
        };
        auto put_chunk = [&](const char* tag, const std::vector<uint8_t>& body) {
            put_u32((uint32_t)body.size());
            std::vector<uint8_t> tagged(tag, tag + 4);
            tagged.insert(tagged.end(), body.begin(), body.end());
            file.write(reinterpret_cast<const char*>(tagged.data()), tagged.size());
            put_u32((uint32_t)mz_crc32(MZ_CRC32_INIT, tagged.data(), tagged.size()));
        };
        file.write("\x89PNG\r\n\x1a\n", 8);
        std::vector<uint8_t> header = {
            uint8_t(width >> 24), uint8_t(width >> 16), uint8_t(width >> 8), uint8_t(width),
            uint8_t(height >> 24), uint8_t(height >> 16), uint8_t(height >> 8), uint8_t(height),
            8, 6, 0, 0, 0, // 8-bit RGBA
        };
        put_chunk("IHDR", header);
        put_chunk("IDAT", compressed);
        put_chunk("IEND", {});
        return file.good();
    }

    std::string utf8(const std::filesystem::path& path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // A mod id from a file name: letters, digits and underscores.
    std::string mod_id_from(const std::string& name) {
        std::string id;
        for (char c : name) {
            id += (std::isalnum((unsigned char)c) != 0) ? c : '_';
        }
        return id;
    }

    nlohmann::json source_of(const std::filesystem::path& htc) {
        std::error_code ec;
        return {
            { "file", utf8(htc.filename()) },
            { "size", std::filesystem::file_size(htc, ec) },
            { "modified", (long long)std::filesystem::last_write_time(htc, ec).time_since_epoch().count() },
        };
    }

    // One texture of a pack, written to the folder as its Rice-named PNG. False if it's skipped (not RGBA8,
    // or its data isn't what its header says).
    bool write_texture(const std::filesystem::path& folder, uint64_t checksum, uint32_t width, uint32_t height, uint32_t gl_format,
        uint16_t pixel_type, uint16_t formatsize, const std::vector<uint8_t>& data, std::vector<uint8_t>& pixels) {
        const size_t rgba_size = size_t(width) * height * 4;
        const uint8_t* rgba = data.data();
        if (gl_format & gl_texfmt_gz) {
            pixels.resize(rgba_size);
            mz_ulong length = (mz_ulong)pixels.size();
            if (mz_uncompress(pixels.data(), &length, data.data(), (mz_ulong)data.size()) != MZ_OK || length != rgba_size) {
                return false;
            }
            rgba = pixels.data();
        }
        else if (data.size() != rgba_size) {
            return false;
        }
        if (pixel_type != gl_unsigned_byte || width == 0 || height == 0) {
            return false;
        }

        const uint32_t crc = uint32_t(checksum), palette = uint32_t(checksum >> 32);
        char name[128];
        if (palette != 0) {
            std::snprintf(name, sizeof(name), "%s#%08X#%d#%d#%08X_all.png", rom_name, crc, formatsize & 0xFF, formatsize >> 8, palette);
        }
        else {
            std::snprintf(name, sizeof(name), "%s#%08X#%d#%d_all.png", rom_name, crc, formatsize & 0xFF, formatsize >> 8);
        }
        return write_png(folder / name, width, height, rgba);
    }

    // A texture's header, after its checksum: the same in both formats.
    struct TextureHeader {
        uint32_t width = 0, height = 0, gl_format = 0, data_size = 0;
        uint16_t texture_format = 0, pixel_type = 0, formatsize = 0;
        uint8_t is_hires = 0;
    };

    template <typename Reader>
    bool read_texture_header(Reader& reader, TextureHeader& header) {
        return reader.read_value(header.width) && reader.read_value(header.height) && reader.read_value(header.gl_format) &&
            reader.read_value(header.texture_format) && reader.read_value(header.pixel_type) && reader.read_value(header.is_hires) &&
            reader.read_value(header.formatsize) && reader.read_value(header.data_size);
    }

    // A plain file, read as the gzip reader is, for the .hts.
    class FileReader {
    public:
        explicit FileReader(const std::filesystem::path& path) : file(path, std::ios::binary) {}
        bool read(void* destination, size_t size) {
            return bool(file.read(static_cast<char*>(destination), (std::streamsize)size));
        }
        template <typename T>
        bool read_value(T& value) {
            return read(&value, sizeof(value));
        }
        bool seek(uint64_t offset) {
            file.clear();
            return bool(file.seekg((std::streamoff)offset, std::ios::beg));
        }
    private:
        std::ifstream file;
    };

    // The textures of a .htc, one after another in its gzip stream. False if it isn't one this can read.
    bool unpack_htc(const std::filesystem::path& htc, const std::filesystem::path& folder, const std::function<void(int)>& progress,
        size_t& written, size_t& skipped) {
        std::error_code ec;
        const uint64_t total = std::max<uint64_t>(std::filesystem::file_size(htc, ec), 1);
        GzipReader reader(htc);
        int32_t version = 0, config = 0;
        if (!reader.read_value(version) || version != htc_version || !reader.read_value(config)) {
            return false;
        }
        std::vector<uint8_t> data, pixels;
        while (true) {
            progress(int(std::min<uint64_t>(reader.bytes_read() * 100 / total, 100)));
            uint64_t checksum = 0;
            TextureHeader header;
            if (!reader.read_value(checksum) || !read_texture_header(reader, header)) {
                break;
            }
            data.resize(header.data_size);
            if (!reader.read(data.data(), data.size())) {
                break;
            }
            if (write_texture(folder, checksum, header.width, header.height, header.gl_format, header.pixel_type, header.formatsize, data, pixels)) {
                written++;
            }
            else {
                skipped++;
            }
        }
        if (reader.failed()) {
            std::fprintf(stderr, "[texture packs] %s: the file ends early or is damaged; unpacked what was readable\n",
                utf8(htc.filename()).c_str());
        }
        return true;
    }

    // The textures of a .hts, found through its index. False if it isn't one this can read.
    bool unpack_hts(const std::filesystem::path& hts, const std::filesystem::path& folder, const std::function<void(int)>& progress,
        size_t& written, size_t& skipped) {
        std::error_code ec;
        const uint64_t file_size = std::filesystem::file_size(hts, ec);
        FileReader reader(hts);
        int32_t version = 0, config = 0;
        int64_t index_offset = 0;
        if (!reader.read_value(version) || version != htc_version || !reader.read_value(config) || !reader.read_value(index_offset) ||
            index_offset <= 0 || (uint64_t)index_offset >= file_size || !reader.seek((uint64_t)index_offset)) {
            return false;
        }
        int32_t count = 0;
        if (!reader.read_value(count) || count <= 0 || (uint64_t)count > (file_size - (uint64_t)index_offset) / 16) {
            return false;
        }
        std::vector<std::pair<uint64_t, uint64_t>> entries; // checksum, offset
        entries.reserve((size_t)count);
        for (int32_t i = 0; i < count; i++) {
            uint64_t checksum = 0;
            int64_t value = 0;
            if (!reader.read_value(checksum) || !reader.read_value(value)) {
                return false;
            }
            entries.push_back({ checksum, (uint64_t)value & 0xFFFFFFFFFFFFull });
        }
        // In the file's order, so it's read front to back.
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

        std::vector<uint8_t> data, pixels;
        for (size_t i = 0; i < entries.size(); i++) {
            progress(int(i * 100 / entries.size()));
            const uint64_t checksum = entries[i].first, offset = entries[i].second;
            TextureHeader header;
            if (offset >= file_size || !reader.seek(offset) || !read_texture_header(reader, header) ||
                header.data_size > file_size - offset) {
                skipped++;
                continue;
            }
            data.resize(header.data_size);
            if (!reader.read(data.data(), data.size())) {
                skipped++;
                continue;
            }
            if (write_texture(folder, checksum, header.width, header.height, header.gl_format, header.pixel_type, header.formatsize, data, pixels)) {
                written++;
            }
            else {
                skipped++;
            }
        }
        return true;
    }

    // progress is given the share of the pack read so far, in percent.
    bool unpack(const std::filesystem::path& htc, const std::filesystem::path& folder, const std::function<void(int)>& progress) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        for (const auto& entry : std::filesystem::directory_iterator{ folder, ec }) {
            if (entry.path().extension() == ".png") {
                std::filesystem::remove(entry.path(), ec);
            }
        }

        size_t written = 0, skipped = 0;
        const bool readable = htc.extension() == ".hts" ? unpack_hts(htc, folder, progress, written, skipped)
                                                        : unpack_htc(htc, folder, progress, written, skipped);
        if (!readable) {
            std::fprintf(stderr, "[texture packs] %s: not a GLideN64 texture pack this can unpack (an older one, or not a .htc or .hts)\n",
                utf8(htc.filename()).c_str());
            return false;
        }

        const std::string stem = utf8(htc.stem());
        const nlohmann::json database = {
            { "configuration", { { "autoPath", "rice" }, { "configurationVersion", 3 }, { "hashVersion", 5 } } },
            { "textures", nlohmann::json::array() },
        };
        const nlohmann::json manifest = {
            { "game_id", "conker" },
            { "id", mod_id_from(stem) },
            { "version", "1.0.0" },
            { "display_name", stem },
            { "description", "Unpacked from the GLideN64 texture pack " + utf8(htc.filename()) + "." },
            { "short_description", "GLideN64 texture pack" },
            { "authors", { "Unknown" } },
            { "minimum_recomp_version", "0.1.0" },
        };
        std::ofstream(folder / "rt64.json") << database.dump(4);
        std::ofstream(folder / "mod.json") << manifest.dump(4);
        std::ofstream(folder / source_record) << source_of(htc).dump(4);
        std::printf("[texture packs] %s: %zu textures unpacked%s\n", utf8(htc.filename()).c_str(), written,
            skipped != 0 ? (", " + std::to_string(skipped) + " skipped (not RGBA8)").c_str() : "");
        return written != 0;
    }
}

namespace {
    // Set while a thread unpacks, until the main thread has finished up after it.
    std::atomic<bool> unpacking = false;
    // What the unpacking thread tells the main thread, which shows it: recompui's prompts are used from
    // the main thread (as its mod installer does), not while the UI thread may be building a menu.
    struct Status {
        std::string file;
        int percent = -1;
        bool finished = false;
        std::vector<std::string> errors;
    };
    std::mutex status_mutex;
    Status status;
    int shown_percent = -1;
    // .htc files that couldn't be unpacked this session, not tried again until the next start (only the
    // unpacking thread uses it).
    std::vector<std::filesystem::path> failed;

    // The .htc files in the mods folder not unpacked yet, or changed since.
    std::vector<std::filesystem::path> packs_to_unpack() {
        std::vector<std::filesystem::path> found;
        const std::filesystem::path mods = recomp::mods::get_mods_directory();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator{ mods, std::filesystem::directory_options::skip_permission_denied, ec }) {
            const std::filesystem::path& htc = entry.path();
            if (!entry.is_regular_file(ec) || (htc.extension() != ".htc" && htc.extension() != ".hts") ||
                std::find(failed.begin(), failed.end(), htc) != failed.end()) {
                continue;
            }
            const std::filesystem::path folder = mods / htc.stem();
            nlohmann::json record;
            {
                std::ifstream record_file(folder / source_record);
                record = nlohmann::json::parse(record_file, nullptr, false);
            }
            if (record != source_of(htc) || !std::filesystem::exists(folder / "rt64.json", ec)) {
                found.push_back(htc);
            }
        }
        return found;
    }

    // On its own thread: unpacks the packs, telling the main thread the progress (update_unpacking).
    void unpack_packs() {
        std::vector<std::string> errors;
        if (packs_to_unpack().empty()) {
            unpacking = false;
            return;
        }
        for (std::vector<std::filesystem::path> packs = packs_to_unpack(); !packs.empty(); packs = packs_to_unpack()) {
            for (const std::filesystem::path& htc : packs) {
                const std::filesystem::path folder = htc.parent_path() / htc.stem();
                const std::string file = utf8(htc.filename());
                std::printf("[texture packs] Unpacking the GLideN64 texture pack %s...\n", file.c_str());
                std::fflush(stdout);
                const bool unpacked = unpack(htc, folder, [&](int percent) {
                    std::lock_guard lock(status_mutex);
                    status.file = file;
                    status.percent = percent;
                });
                if (!unpacked) {
                    failed.push_back(htc);
                    errors.push_back(file);
                }
            }
        }
        std::lock_guard lock(status_mutex);
        status.finished = true;
        status.errors = std::move(errors);
    }
}

std::string conker::texture_packs::gliden64_pack_id(const std::filesystem::path& htc) {
    return mod_id_from(utf8(htc.stem()));
}

void conker::texture_packs::unpack_gliden64_packs() {
    if (ultramodern::is_game_started() || unpacking.exchange(true)) {
        return;
    }
    std::thread(unpack_packs).detach();
}

void conker::texture_packs::update_unpacking() {
    Status now;
    {
        std::lock_guard lock(status_mutex);
        if (status.file.empty() && !status.finished) {
            return;
        }
        now = status;
        if (status.finished) {
            status = {};
        }
    }
    if (!now.file.empty() && now.percent != shown_percent) {
        shown_percent = now.percent;
        recompui::open_notification("Unpacking a Texture Pack",
            now.file + ": " + std::to_string(now.percent) + "%. A GLideN64 texture pack is unpacked once, "
            "the first time the game finds it; a large one takes a minute or so.");
    }
    if (!now.finished) {
        return;
    }

    if (shown_percent != -1) {
        recompui::close_prompt();
        shown_percent = -1;
    }
    // Has the runtime open the packs (turning a new one on, as any new mod) and applies the Texture Packs
    // setting again. The mods can't change once the game has started (they're opened at the next start).
    if (!ultramodern::is_game_started()) {
        recomp::mods::scan_mods();
        apply();
        recompui::update_mod_list(false);
    }
    if (!now.errors.empty()) {
        std::string list;
        for (const std::string& file : now.errors) {
            list += (list.empty() ? "" : ", ") + file;
        }
        recompui::open_info_prompt("Texture Pack", list + " couldn't be unpacked: it isn't a GLideN64 texture "
            "cache (.htc) of a recent GLideN64 version, or it's damaged.", "OK", {}, recompui::ButtonStyle::Tertiary);
    }
    unpacking = false;
}
