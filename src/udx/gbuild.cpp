// gbuild: builds a snapshot from consolidated edges and returns it in chunks.
//   vgraph.gbuild(src, dst, weight, max_epoch USING PARAMETERS graph='g', directed=true)
//       OVER(ORDER BY src, dst)
// Output (chunk_no, chunk, node_count, edge_count, max_epoch, format_version).
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

            vgraph::GraphBuilder builder(directed, false);
            vint max_epoch = 0, rows = 0;
            do {
                if (inputReader.isNull(0) || inputReader.isNull(1))
                    vt_report_error(0, "%s: graph '%s': src and dst must not be NULL", FN, graph.c_str());
                float weight = 1.0f;
                if (!inputReader.isNull(2)) {
                    builder.enable_weights();
                    weight = (float)inputReader.getFloatRef(2);
                }
                builder.add_edge(inputReader.getIntRef(0), inputReader.getIntRef(1), weight);
                if (!inputReader.isNull(3)) max_epoch = std::max(max_epoch, inputReader.getIntRef(3));
                if ((++rows & 0xFFFFF) == 0 && isCanceled()) return;
            } while (inputReader.next());

            vgraph::SnapshotBuffer buffer;
            builder.finish(max_epoch, buffer);
            const vgraph::Csr csr = vgraph::snapshot_open(buffer.data(), buffer.size(), false);

            const char *bytes = reinterpret_cast<const char *>(buffer.data());
            vint chunk_no = 0;
            for (std::uint64_t off = 0; off < buffer.size(); off += vgraph::CHUNK_BYTES, ++chunk_no) {
                const std::uint64_t len = std::min<std::uint64_t>(vgraph::CHUNK_BYTES, buffer.size() - off);
                outputWriter.setInt(0, chunk_no);
                outputWriter.getStringRef(1).copy(bytes + off, len);
                outputWriter.setInt(2, (vint)csr.node_count);
                outputWriter.setInt(3, (vint)csr.edge_count);
                outputWriter.setInt(4, csr.max_epoch);
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
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        argTypes.addInt();
        argTypes.addInt();
        argTypes.addFloat();
        argTypes.addInt();
        returnType.addInt();
        returnType.addLongVarbinary();
        for (int i = 0; i < 4; ++i) returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("chunk_no");
        outputTypes.addLongVarbinary((int32)vgraph::CHUNK_BYTES, "chunk");
        outputTypes.addInt("node_count");
        outputTypes.addInt("edge_count");
        outputTypes.addInt("max_epoch");
        outputTypes.addInt("format_version");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        parameterTypes.addVarchar(128, "graph");
        parameterTypes.addBool("directed");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GBuild>(srvInterface.allocator); }
};

RegisterFactory(GBuildFactory);
