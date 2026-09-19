#include "cache.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vgraph {

namespace {

[[noreturn]] void fail(const std::string &what, const std::string &path, bool with_errno = true)
{
    std::string msg = what + " " + path;
    if (with_errno) msg += std::string(": ") + std::strerror(errno);
    throw std::runtime_error(msg);
}

void make_dir(const std::string &path)
{
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) fail("cannot create directory", path);
}

// True if the file starts with the vgraph magic.
bool file_has_magic(const std::string &path)
{
    std::uint8_t head[sizeof(SNAPSHOT_MAGIC)];
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const ssize_t got = ::read(fd, head, sizeof(head));
    ::close(fd);
    return got == static_cast<ssize_t>(sizeof(head)) && snapshot_has_magic(head, sizeof(head));
}

// Writes a small text file atomically: temp file, then rename.
void write_atomically(const std::string &path, const std::string &content)
{
    const std::string tmp = path + ".tmp." + std::to_string(getpid());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) fail("cannot create", tmp);
    const bool ok = ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size()) &&
                    ::fsync(fd) == 0;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        fail("cannot write", path);
    }
}

} // namespace

bool valid_graph_name(const std::string &graph)
{
    if (graph.empty() || graph.size() > 64) return false;
    for (char c : graph)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

static std::string graph_dir(const std::string &cache_dir, const std::string &graph)
{
    if (!valid_graph_name(graph))
        throw std::runtime_error("graph name '" + graph + "' is not valid: use letters, digits and underscore");
    if (cache_dir.empty() || cache_dir[0] != '/')
        throw std::runtime_error("cache_dir '" + cache_dir + "' must be an absolute path");
    return cache_dir + "/" + graph;
}

std::string snapshot_path(const std::string &cache_dir, const std::string &graph, std::int64_t snapshot_id)
{
    return graph_dir(cache_dir, graph) + "/" + std::to_string(snapshot_id) + ".vg";
}

bool read_active(const std::string &cache_dir, const std::string &graph, std::int64_t &snapshot_id)
{
    const std::string path = graph_dir(cache_dir, graph) + "/ACTIVE";
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    long long id = 0;
    const bool ok = std::fscanf(f, "%lld", &id) == 1;
    std::fclose(f);
    snapshot_id = id;
    return ok;
}

std::vector<std::string> list_cached_graphs(const std::string &cache_dir)
{
    std::vector<std::string> graphs;
    DIR *d = opendir(cache_dir.c_str());
    if (!d) return graphs;
    while (dirent *e = readdir(d))
        if (valid_graph_name(e->d_name)) graphs.push_back(e->d_name);
    closedir(d);
    return graphs;
}

// ---- MappedSnapshot

MappedSnapshot::~MappedSnapshot()
{
    if (map_) munmap(map_, size_);
}

void MappedSnapshot::open(const std::string &path, bool verify_checksum)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) fail("cannot open", path);
    struct stat st;
    if (fstat(fd, &st) != 0) { ::close(fd); fail("cannot stat", path); }
    if (st.st_size == 0) { ::close(fd); fail("empty snapshot file", path, false); }
    void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) fail("cannot mmap", path);
    if (map_) munmap(map_, size_);
    map_ = m;
    size_ = st.st_size;
    path_ = path;
    try {
        csr_ = snapshot_open(static_cast<const std::uint8_t *>(map_), size_, verify_checksum);
    } catch (const std::runtime_error &e) {
        throw std::runtime_error(std::string(e.what()) + " in " + path);
    }
}

void MappedSnapshot::open_active(const std::string &cache_dir, const std::string &graph)
{
    std::int64_t id = 0;
    if (!read_active(cache_dir, graph, id))
        throw std::runtime_error("no snapshot cache for graph '" + graph + "' in " + cache_dir + ": run gload");
    open(snapshot_path(cache_dir, graph, id), false);
    snapshot_id_ = id;
}

// ---- CacheWriter

CacheWriter::~CacheWriter() { discard(); }

void CacheWriter::discard()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        ::unlink(tmp_path_.c_str());
    }
}

void CacheWriter::begin(const std::string &cache_dir, const std::string &graph, std::int64_t snapshot_id)
{
    dir_ = graph_dir(cache_dir, graph);
    cache_dir_ = cache_dir;
    graph_ = graph;
    make_dir(cache_dir);
    make_dir(dir_);
    snapshot_id_ = snapshot_id;
    final_path_ = snapshot_path(cache_dir, graph, snapshot_id);
    tmp_path_ = final_path_ + ".tmp." + std::to_string(getpid());
    fd_ = ::open(tmp_path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd_ < 0) fail("cannot create", tmp_path_);
}

void CacheWriter::write_chunk(std::int64_t chunk_no, const char *data, std::uint64_t len)
{
    if (chunk_no < 0 || len == 0 || len > CHUNK_BYTES)
        throw std::runtime_error("bad chunk " + std::to_string(chunk_no) + " of " + std::to_string(len) + " bytes");
    std::uint64_t offset = static_cast<std::uint64_t>(chunk_no) * CHUNK_BYTES;
    std::uint64_t left = len;
    while (left > 0) {
        const ssize_t n = pwrite(fd_, data, left, static_cast<off_t>(offset));
        if (n <= 0) fail("cannot write", tmp_path_);
        data += n;
        offset += n;
        left -= n;
    }
    bytes_written_ += len;
    if (offset > end_offset_) end_offset_ = offset;
}

std::uint64_t CacheWriter::commit()
{
    // Every byte written exactly once: no chunk missing, none twice.
    if (bytes_written_ != end_offset_)
        throw std::runtime_error("chunks are missing or duplicated: " + std::to_string(bytes_written_) +
                                 " bytes received for a file of " + std::to_string(end_offset_));
    if (::fsync(fd_) != 0) fail("cannot sync", tmp_path_);
    {
        MappedSnapshot check;
        check.open(tmp_path_, true);
    }
    std::int64_t previous = 0;
    const bool had_previous = read_active(cache_dir_, graph_, previous);

    if (::rename(tmp_path_.c_str(), final_path_.c_str()) != 0) fail("cannot rename to", final_path_);
    ::close(fd_);
    fd_ = -1;
    write_atomically(dir_ + "/ACTIVE", std::to_string(snapshot_id_) + "\n");

    // Keep the new and the previous snapshot. Remove other vgraph files only.
    const std::string keep_new = std::to_string(snapshot_id_) + ".vg";
    const std::string keep_old = had_previous ? std::to_string(previous) + ".vg" : keep_new;
    if (DIR *d = opendir(dir_.c_str())) {
        while (dirent *e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == keep_new || name == keep_old || name == "ACTIVE" || name == "." || name == "..") continue;
            const std::string path = dir_ + "/" + name;
            struct stat st;
            if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && file_has_magic(path)) ::unlink(path.c_str());
        }
        closedir(d);
    }
    return end_offset_;
}

} // namespace vgraph
