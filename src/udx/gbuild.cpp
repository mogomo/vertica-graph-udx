// gbuild: builds a snapshot from consolidated edges and returns it in chunks.
//   vgraph.gbuild(src, dst         USING PARAMETERS graph='g', directed=true, max_ver=0) OVER(ORDER BY src, dst)
//   vgraph.gbuild(src, dst, weight USING PARAMETERS ...)                                 OVER(ORDER BY src, dst)
// Two signatures, so that an unweighted graph does not pay for a weight column:
// every input column costs 8 bytes per edge on the way into the function.
// max_ver is the journal watermark. It is a parameter, not a column, for the same reason.
// Output (byte_offset, chunk, node_count, edge_count, max_ver, format_version).
// Thin adapter around src/engine/builder.h.
#include "Vertica.h"
#include "../engine/builder.h"
#include "../engine/cache.h"
#include "../engine/version.h"

#include <algorithm>

using namespace Vertica;

static const char *const FN = "gbuild";

class GBuild : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            ParamReader params = srvInterface.getParamReader();
            if (!params.containsParameter("graph"))
                vt_report_error(0, "%s: parameter graph is required", FN);
            const std::string graph = params.getStringRef("graph").str();
            if (!vgraph::valid_graph_name(graph))
                vt_report_error(0, "%s: graph name '%s' is not valid: use letters, digits and underscore",
                                FN, graph.c_str());
            const bool directed = !params.containsParameter("directed") ||
                                  params.getBoolRef("directed") == vbool_true;

            const vint max_ver = params.containsParameter("max_ver") ? params.getIntRef("max_ver") : 0;
            const bool has_weight = inputReader.getNumCols() > 2;

            vgraph::GraphBuilder builder(directed, has_weight);
            vint rows = 0;
            do {
                if (inputReader.isNull(0) || inputReader.isNull(1))
                    vt_report_error(0, "%s: graph '%s': src and dst must not be NULL", FN, graph.c_str());
                const float weight = (has_weight && !inputReader.isNull(2)) ? (float)inputReader.getFloatRef(2) : 1.0f;
                builder.add_edge(inputReader.getIntRef(0), inputReader.getIntRef(1), weight);
                if ((++rows & 0xFFFFF) == 0 && isCanceled()) return;
            } while (inputReader.next());

            vgraph::SnapshotBuffer buffer;
            builder.finish(max_ver, buffer);
            const vgraph::Csr csr = vgraph::snapshot_open(buffer.data(), buffer.size(), false);

            const char *bytes = reinterpret_cast<const char *>(buffer.data());
            for (std::uint64_t off = 0; off < buffer.size(); off += vgraph::CHUNK_BYTES) {
                const std::uint64_t len = std::min<std::uint64_t>(vgraph::CHUNK_BYTES, buffer.size() - off);
                outputWriter.setInt(0, (vint)off);
                outputWriter.getStringRef(1).copy(bytes + off, len);
                outputWriter.setInt(2, (vint)csr.node_count);
                outputWriter.setInt(3, (vint)csr.edge_count);
                outputWriter.setInt(4, csr.max_ver);
                outputWriter.setInt(5, vgraph::FORMAT_VERSION);
                outputWriter.next();
                if (isCanceled()) return;
            }
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GBuildFactory : public TransformFunctionFactory
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
        for (int i = 0; i < 4; ++i) returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("byte_offset");
        outputTypes.addLongVarbinary((int32)vgraph::CHUNK_BYTES, "chunk");
        outputTypes.addInt("node_count");
        outputTypes.addInt("edge_count");
        outputTypes.addInt("max_ver");
        outputTypes.addInt("format_version");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        parameterTypes.addVarchar(128, "graph");
        parameterTypes.addBool("directed");
        parameterTypes.addInt("max_ver");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GBuild>(srvInterface.allocator); }
};

class GBuildWeightedFactory : public GBuildFactory
{
    virtual bool weighted() const { return true; }
};

RegisterFactory(GBuildFactory);
RegisterFactory(GBuildWeightedFactory);
