// gload: writes the snapshot cache file on every node and makes it active.
//   vgraph.gload(byte_offset, chunk USING PARAMETERS graph='g', snapshot_id=7) OVER(PARTITION NODES)
// Output (node_name, snapshot_id, bytes, status). Idempotent: run it again any time.
// Thin adapter around src/engine/cache.h.
#include "udx_common.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gload";

class GLoad : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        const std::string node = srvInterface.getCurrentNodeName();
        try {
            ParamReader params = srvInterface.getParamReader();
            if (!params.containsParameter("graph") || !params.containsParameter("snapshot_id"))
                vt_report_error(0, "%s: parameters graph and snapshot_id are required", FN);
            const std::string graph = params.getStringRef("graph").str();
            const vint snapshot_id = params.getIntRef("snapshot_id");

            vgraph::CacheWriter writer;
            writer.begin(resolve_cache_dir(srvInterface), graph, snapshot_id);
            do {
                if (inputReader.isNull(0) || inputReader.getStringRef(1).isNull())
                    vt_report_error(0, "%s: graph '%s' on %s: NULL byte_offset or chunk", FN, graph.c_str(), node.c_str());
                const VString &chunk = inputReader.getStringRef(1);
                writer.write_at(inputReader.getIntRef(0), chunk.data(), chunk.length());
                if (isCanceled()) return;
            } while (inputReader.next());
            const std::uint64_t bytes = writer.commit();

            outputWriter.getStringRef(0).copy(node);
            outputWriter.setInt(1, snapshot_id);
            outputWriter.setInt(2, (vint)bytes);
            outputWriter.getStringRef(3).copy("loaded");
            outputWriter.next();
        } catch (std::exception &e) {
            vt_report_error(0, "%s: on %s: %s", FN, node.c_str(), e.what());
        }
    }
};

class GLoadFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        argTypes.addInt();
        argTypes.addLongVarbinary();
        returnType.addVarchar();
        returnType.addInt();
        returnType.addInt();
        returnType.addVarchar();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addVarchar(128, "node_name");
        outputTypes.addInt("snapshot_id");
        outputTypes.addInt("bytes");
        outputTypes.addVarchar(32, "status");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("snapshot_id");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GLoad>(srvInterface.allocator); }
};

RegisterFactory(GLoadFactory);
