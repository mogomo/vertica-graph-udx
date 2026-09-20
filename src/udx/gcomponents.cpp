// gcomponents: connected components (weakly connected on directed graphs).
// Output (node, component); component is the smallest node id in it.
// gcomponents_count: the same, returns one row per component: (component, nodes).
// Thin adapter around src/engine/cc.h.
#include "udx_common.h"
#include "../engine/cc.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gcomponents";
static const char *const FN_COUNT = "gcomponents_count";

class GComponents : public TransformFunction
{
public:
    explicit GComponents(bool count_only) : count_only_(count_only) {}

private:
    bool count_only_;
    int threads_ = 4;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    { threads_ = read_threads(count_only_ ? FN_COUNT : FN, srvInterface); }

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            const char *fn = count_only_ ? FN_COUNT : FN;
            QueryGraph q;
            if (!q.read(fn, srvInterface, inputReader, [this]() { return isCanceled(); })) return;
            q.run([&](const auto &graph) {
                std::vector<vgraph::pos_t> component;
                vgraph::connected_components(graph, component, threads_);
                if (count_only_) {
                    // component[p] is a position: count per position, report the components in position order
                    std::vector<std::int64_t> nodes(graph.node_count(), 0);
                    for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) ++nodes[component[p]];
                    for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) {
                        if (nodes[p] == 0) continue;
                        outputWriter.setInt(0, graph.id_of(p));
                        outputWriter.setInt(1, nodes[p]);
                        outputWriter.next();
                    }
                    return;
                }
                for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) {
                    outputWriter.setInt(0, graph.id_of(p));
                    outputWriter.setInt(1, graph.id_of(component[p]));
                    outputWriter.next();
                    if ((p & 0xFFFFF) == 0 && isCanceled()) return;
                }
            });
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", count_only_ ? FN_COUNT : FN, e.what());
        }
    }
};

class GComponentsFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        add_query_input(argTypes);
        returnType.addInt();
        returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("node");
        outputTypes.addInt("component");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("threads");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GComponents>(srvInterface.allocator, false); }
};

class GComponentsCountFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        add_query_input(argTypes);
        returnType.addInt();
        returnType.addInt();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("component");
        outputTypes.addInt("nodes");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("threads");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GComponents>(srvInterface.allocator, true); }
};

RegisterFactory(GComponentsFactory);
RegisterFactory(GComponentsCountFactory);
