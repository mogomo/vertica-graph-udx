// gbuild_mapped: the streaming build, for graphs that do not fit the memory of gbuild.
// Vertica prepares a node map and the mapped, unique, sorted edges; every call writes one section
// of the snapshot from a sorted stream. Memory: one chunk. See src/engine/section_writer.h.
//
//   vgraph.gbuild_mapped(a, b [, w] USING PARAMETERS graph='g', section='out_nbrs', node_count=N,
//                        edge_count=E, directed=true, weighted=false, in_equals_out=false, max_ver=0)
//          OVER(ORDER BY a, b)
//   Output (byte_offset, chunk). The last row has byte_offset = -1 - section number and the 8 byte
//   checksum part of the section as its chunk.
//
//   vgraph.gbuild_header(byte_offset, chunk USING PARAMETERS same counts and flags) OVER()
//   Input: the checksum rows of all sections. Output: the header, (0, 128 bytes).
#include "Vertica.h"
#include "../engine/cache.h"
#include "../engine/section_writer.h"

#include <cstring>

using namespace Vertica;

namespace {

vgraph::SnapshotHeader header_from_parameters(const char *fn, ParamReader &params)
{
    const char *const needed[] = {"graph", "node_count", "edge_count"};
    for (const char *name : needed)
        if (!params.containsParameter(name)) vt_report_error(0, "%s: parameter %s is required", fn, name);
    const std::string graph = params.getStringRef("graph").str();
    if (!vgraph::valid_graph_name(graph))
        vt_report_error(0, "%s: graph name '%s' is not valid: use letters, digits and underscore", fn, graph.c_str());
    auto flag = [&](const char *name, bool fallback) {
        return params.containsParameter(name) ? params.getBoolRef(name) == vbool_true : fallback;
    };
    const vint n = params.getIntRef("node_count"), e = params.getIntRef("edge_count");
    if (n < 0 || e < 0 || (std::uint64_t)n >= vgraph::NO_POS)
        vt_report_error(0, "%s: graph '%s': node_count %lld does not fit (at most 2^32 - 2 nodes)", fn, graph.c_str(), (long long)n);
    const std::uint32_t flags = (flag("directed", true) ? vgraph::FLAG_DIRECTED : 0u) |
                                (flag("weighted", false) ? vgraph::FLAG_WEIGHTED : 0u) |
                                (flag("in_equals_out", false) ? vgraph::FLAG_IN_EQUALS_OUT : 0u);
    return vgraph::make_header(n, e, flags, params.containsParameter("max_ver") ? params.getIntRef("max_ver") : 0);
}

void add_header_parameters(SizedColumnTypes &parameterTypes)
{
    parameterTypes.addVarchar(128, "graph");
    parameterTypes.addInt("node_count");
    parameterTypes.addInt("edge_count");
    parameterTypes.addBool("directed");
    parameterTypes.addBool("weighted");
    parameterTypes.addBool("in_equals_out");
    parameterTypes.addInt("max_ver");
}

} // namespace

class GBuildMapped : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface, PartitionReader &inputReader, PartitionWriter &outputWriter)
    {
        static const char *const FN = "gbuild_mapped";
        try {
            ParamReader params = srvInterface.getParamReader();
            const vgraph::SnapshotHeader header = header_from_parameters(FN, params);
            if (!params.containsParameter("section")) vt_report_error(0, "%s: parameter section is required", FN);
            const std::string name = params.getStringRef("section").str();
            vgraph::Section section;
            if (!vgraph::parse_section(name, section))
                vt_report_error(0, "%s: unknown section '%s'", FN, name.c_str());
            const bool has_weight = inputReader.getNumCols() > 2;

            vgraph::SectionWriter writer(header, section, vgraph::CHUNK_BYTES,
                [&](std::uint64_t offset, const char *data, std::uint64_t len) {
                    outputWriter.setInt(0, (vint)offset);
                    outputWriter.getStringRef(1).copy(data, len);
                    outputWriter.next();
                });
            vint rows = 0;
            do {
                if (inputReader.isNull(0) || inputReader.isNull(1))
                    vt_report_error(0, "%s: section %s: NULL in the input", FN, name.c_str());
                const float w = (has_weight && !inputReader.isNull(2)) ? (float)inputReader.getFloatRef(2) : 1.0f;
                writer.add(inputReader.getIntRef(0), inputReader.getIntRef(1), w);
                if ((++rows & 0xFFFFF) == 0 && isCanceled()) return;
            } while (inputReader.next());

            const std::uint64_t part = writer.finish();
            outputWriter.setInt(0, -1 - (vint)section);
            outputWriter.getStringRef(1).copy(reinterpret_cast<const char *>(&part), sizeof(part));
            outputWriter.next();
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GBuildMappedFactory : public TransformFunctionFactory
{
protected:
    virtual bool weighted() const { return false; }

    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        argTypes.addInt();
        argTypes.addInt();
        if (weighted()) argTypes.addFloat();
        returnType.addInt();
        returnType.addLongVarbinary();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes, SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("byte_offset");
        outputTypes.addLongVarbinary((int32)vgraph::CHUNK_BYTES, "chunk");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_header_parameters(parameterTypes);
        parameterTypes.addVarchar(32, "section");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GBuildMapped>(srvInterface.allocator); }
};

class GBuildMappedWeightedFactory : public GBuildMappedFactory
{
    virtual bool weighted() const { return true; }
};

class GBuildHeader : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface, PartitionReader &inputReader, PartitionWriter &outputWriter)
    {
        static const char *const FN = "gbuild_header";
        try {
            ParamReader params = srvInterface.getParamReader();
            const vgraph::SnapshotHeader header = header_from_parameters(FN, params);
            std::vector<std::uint64_t> parts;
            do {
                if (inputReader.isNull(0) || inputReader.getIntRef(0) >= 0) continue;      // only checksum rows
                const VString &chunk = inputReader.getStringRef(1);
                if (chunk.isNull() || chunk.length() != sizeof(std::uint64_t))
                    vt_report_error(0, "%s: checksum row %lld is damaged", FN, (long long)inputReader.getIntRef(0));
                std::uint64_t part;
                std::memcpy(&part, chunk.data(), sizeof(part));
                parts.push_back(part);
            } while (inputReader.next());

            // One checksum row per section the flags ask for.
            const bool reverse = (header.flags & vgraph::FLAG_DIRECTED) && !(header.flags & vgraph::FLAG_IN_EQUALS_OUT);
            const bool weights = (header.flags & vgraph::FLAG_WEIGHTED) != 0;
            const size_t wanted = 3 + (reverse ? 2 : 0) + (weights ? 1 : 0) + ((weights && reverse) ? 1 : 0);
            if (parts.size() != wanted)
                vt_report_error(0, "%s: %zu sections were written, the snapshot needs %zu", FN, parts.size(), wanted);

            const vgraph::SnapshotHeader done = vgraph::finish_header(header, parts);
            outputWriter.setInt(0, 0);
            outputWriter.getStringRef(1).copy(reinterpret_cast<const char *>(&done), sizeof(done));
            outputWriter.next();
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GBuildHeaderFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        argTypes.addInt();
        argTypes.addLongVarbinary();
        returnType.addInt();
        returnType.addLongVarbinary();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes, SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("byte_offset");
        outputTypes.addLongVarbinary(256, "chunk");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    { add_header_parameters(parameterTypes); }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GBuildHeader>(srvInterface.allocator); }
};

RegisterFactory(GBuildMappedFactory);
RegisterFactory(GBuildMappedWeightedFactory);
RegisterFactory(GBuildHeaderFactory);
