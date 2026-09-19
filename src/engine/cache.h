// vgraph engine: per-node snapshot cache.
//   <cache_dir>/<graph>/<snapshot_id>.vg   snapshot file, read with mmap
//   <cache_dir>/<graph>/ACTIVE             text file with the active snapshot id
// POSIX only. No Vertica includes.
#ifndef VGRAPH_ENGINE_CACHE_H
#define VGRAPH_ENGINE_CACHE_H

#include "snapshot.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vgraph {

constexpr const char *DEFAULT_CACHE_DIR = "/tmp/vgraph";

// Snapshots travel through Vertica in chunks of this size (the last one is shorter).
constexpr std::uint64_t CHUNK_BYTES = 8u * 1024u * 1024u;

// Graph names become directory names: letters, digits and underscore, 1 to 64 characters.
bool valid_graph_name(const std::string &graph);

std::string snapshot_path(const std::string &cache_dir, const std::string &graph, std::int64_t snapshot_id);

// Returns false when the graph has no ACTIVE file in this cache.
bool read_active(const std::string &cache_dir, const std::string &graph, std::int64_t &snapshot_id);

// Names of the graphs that have a directory in this cache.
std::vector<std::string> list_cached_graphs(const std::string &cache_dir);

// A snapshot file mapped read-only. Throws std::runtime_error with the cause.
class MappedSnapshot {
public:
    MappedSnapshot() = default;
    ~MappedSnapshot();
    MappedSnapshot(const MappedSnapshot &) = delete;
    MappedSnapshot &operator=(const MappedSnapshot &) = delete;

    void open(const std::string &path, bool verify_checksum);
    // Opens the active snapshot of a graph.
    void open_active(const std::string &cache_dir, const std::string &graph);

    const Csr &csr() const { return csr_; }
    std::int64_t snapshot_id() const { return snapshot_id_; }
    const std::string &path() const { return path_; }
    std::uint64_t size() const { return size_; }

private:
    void *map_ = nullptr;
    std::uint64_t size_ = 0;
    std::int64_t snapshot_id_ = 0;
    std::string path_;
    Csr csr_;
};

// Writes one snapshot file from chunks that may arrive in any order, then
// makes it the active one. A failed or abandoned load leaves the cache as it was.
class CacheWriter {
public:
    CacheWriter() = default;
    ~CacheWriter();
    CacheWriter(const CacheWriter &) = delete;
    CacheWriter &operator=(const CacheWriter &) = delete;

    void begin(const std::string &cache_dir, const std::string &graph, std::int64_t snapshot_id);
    void write_chunk(std::int64_t chunk_no, const char *data, std::uint64_t len);

    // Verifies the file (size, structure, checksum), renames it into place,
    // flips ACTIVE (write temp, rename) and removes snapshot files other than
    // the new and the previously active one. Only files in the graph's
    // directory that start with the vgraph magic are ever removed.
    // Returns the snapshot size in bytes.
    std::uint64_t commit();

private:
    void discard();
    int fd_ = -1;
    std::string cache_dir_, graph_, dir_, tmp_path_, final_path_;
    std::int64_t snapshot_id_ = 0;
    std::uint64_t bytes_written_ = 0, end_offset_ = 0;
};

} // namespace vgraph

#endif
