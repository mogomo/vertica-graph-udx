// ginfo: what each node has in its snapshot cache.
//   vgraph.ginfo([USING PARAMETERS graph='g']) OVER(PARTITION NODES) FROM vgraph.probe
// Output (node_name, graph, snapshot_id, max_epoch, node_count, edge_count, cache_file, loaded).
#include "udx_common.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "ginfo";

class GInfo : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        const std::string node = srvInterface.getCurrentNodeName();
        try {
            ParamReader params = srvInterface.getParamReader();
            const std::string cache_dir = resolve_cache_dir(srvInterface);
            std::vector<std::string> graphs;
            if (params.containsParameter("graph")) graphs.push_back(params.getStringRef("graph").str());
            else graphs = vgraph::list_cached_graphs(cache_dir);

            for (const std::string &graph : graphs) {
                outputWriter.getStringRef(0).copy(node);
                outputWriter.getStringRef(1).copy(graph);
                try {
                    vgraph::MappedSnapshot snap;
                    snap.open_active(cache_dir, graph);
                    outputWriter.setInt(2, snap.snapshot_id());
                    outputWriter.setInt(3, snap.csr().max_epoch);
                    outputWriter.setInt(4, (vint)snap.csr().node_count);
                    outputWriter.setInt(5, (vint)snap.csr().edge_count);
                    outputWriter.getStringRef(6).copy(snap.path());
                    outputWriter.setBool(7, vbool_true);
                } catch (std::runtime_error &e) {
                    // Not loaded or damaged: say why in the cache_file column.
                    for (int c = 2; c <= 5; ++c) outputWriter.setNull(c);
                    outputWriter.getStringRef(6).copy(std::string(e.what()).substr(0, 1024));
                    outputWriter.setBool(7, vbool_false);
                }
                outputWriter.next();
            }
            if (graphs.empty()) {
                outputWriter.getStringRef(0).copy(node);
                for (int c = 1; c <= 5; ++c) outputWriter.setNull(c);
                outputWriter.getStringRef(6).copy("no graphs in " + cache_dir);
                outputWriter.setBool(7, vbool_false);
                outputWriter.next();
            }
        } catch (std::exception &e) {
            vt_report_error(0, "%s: on %s: %s", FN, node.c_str(), e.what());
        }
    }
};

class GInfoFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        returnType.addVarchar();
        returnType.addVarchar();
        for (int i = 0; i < 4; ++i) returnType.addInt();
        returnType.addVarchar();
        returnType.addBool();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addVarchar(128, "node_name");
        outputTypes.addVarchar(128, "graph");
        outputTypes.addInt("snapshot_id");
        outputTypes.addInt("max_epoch");
        outputTypes.addInt("node_count");
        outputTypes.addInt("edge_count");
        outputTypes.addVarchar(1200, "cache_file");
        outputTypes.addBool("loaded");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    { add_common_query_parameters(parameterTypes); }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GInfo>(srvInterface.allocator); }
};

RegisterFactory(GInfoFactory);
