// gpagerank: PageRank. Output (node, rank); ranks sum to 1.
// Thin adapter around src/engine/pagerank.h.
#include "udx_common.h"
#include "../engine/pagerank.h"

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gpagerank";

class GPagerank : public TransformFunction
{
    vint iterations_ = 20;
    vfloat damping_ = 0.85;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        ParamReader params = srvInterface.getParamReader();
        if (params.containsParameter("iterations")) iterations_ = params.getIntRef("iterations");
        if (params.containsParameter("damping")) damping_ = params.getFloatRef("damping");
        if (iterations_ < 1 || iterations_ > 1000)
            vt_report_error(0, "%s: iterations must be between 1 and 1000", FN);
        if (!(damping_ >= 0.0 && damping_ <= 1.0))
            vt_report_error(0, "%s: damping must be between 0 and 1", FN);
    }

    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            QueryGraph q;
            if (!q.read(FN, srvInterface, inputReader, [this]() { return isCanceled(); })) return;
            q.run([&](const auto &graph) {
                std::vector<double> rank;
                vgraph::pagerank(graph, (int)iterations_, damping_, rank);
                for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) {
                    outputWriter.setInt(0, graph.id_of(p));
                    outputWriter.setFloat(1, rank[p]);
                    outputWriter.next();
                    if ((p & 0xFFFFF) == 0 && isCanceled()) return;
                }
            });
        } catch (std::exception &e) {
            vt_report_error(0, "%s: %s", FN, e.what());
        }
    }
};

class GPagerankFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface, ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        add_query_input(argTypes);
        returnType.addInt();
        returnType.addFloat();
    }

    virtual void getReturnType(ServerInterface &srvInterface, const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addInt("node");
        outputTypes.addFloat("rank");
    }

    virtual void getParameterType(ServerInterface &srvInterface, SizedColumnTypes &parameterTypes)
    {
        add_common_query_parameters(parameterTypes);
        parameterTypes.addInt("iterations");
        parameterTypes.addFloat("damping");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GPagerank>(srvInterface.allocator); }
};

RegisterFactory(GPagerankFactory);
