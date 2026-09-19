// gcomponents: connected components (weakly connected on directed graphs).
// Output (node, component); component is the smallest node id in it.
// Thin adapter around src/engine/cc.h.
#include "udx_common.h"
#include "../engine/cc.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gcomponents";

class GComponents : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            QueryGraph q;
            if (!q.read(FN, srvInterface, inputReader, [this]() { return isCanceled(); })) return;
            q.run([&](const auto &graph) {
                std::vector<vgraph::pos_t> component;
                vgraph::connected_components(graph, component);
                for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) {
                    outputWriter.setInt(0, graph.id_of(p));
                    outputWriter.setInt(1, graph.id_of(component[p]));
                    outputWriter.next();
                    if ((p & 0xFFFFF) == 0 && isCanceled()) return;
                }
            });
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
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
    { add_common_query_parameters(parameterTypes); }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GComponents>(srvInterface.allocator); }
};

RegisterFactory(GComponentsFactory);
