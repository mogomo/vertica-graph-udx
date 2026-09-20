// gpagerank: PageRank. Output (node, rank); ranks sum to 1 over the whole graph.
// top=N returns only the N highest ranks: a graph has millions of nodes, a report needs a few.
// Thin adapter around src/engine/pagerank.h.
#include "udx_common.h"
#include "../engine/pagerank.h"

#include <algorithm>

using namespace Vertica;
using namespace vgraph_udx;

static const char *const FN = "gpagerank";

class GPagerank : public TransformFunction
{
    vint iterations_ = 20;
    vfloat damping_ = 0.85;
    vint top_ = 0;
    int threads_ = 4;

    virtual void setup(ServerInterface &srvInterface, const SizedColumnTypes &argTypes)
    {
        ParamReader params = srvInterface.getParamReader();
        if (params.containsParameter("iterations")) iterations_ = params.getIntRef("iterations");
        if (params.containsParameter("damping")) damping_ = params.getFloatRef("damping");
        if (params.containsParameter("top")) top_ = params.getIntRef("top");
        threads_ = read_threads(FN, srvInterface);
        if (top_ < 0) vt_report_error(0, "%s: top must not be negative", FN);
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
                vgraph::pagerank(graph, (int)iterations_, damping_, rank, threads_);
                if (top_ > 0 && top_ < (vint)graph.node_count()) {
                    // the top N, highest first; equal ranks by position
                    std::vector<vgraph::pos_t> order(graph.node_count());
                    for (vgraph::pos_t p = 0; p < graph.node_count(); ++p) order[p] = p;
                    auto higher = [&rank](vgraph::pos_t a, vgraph::pos_t b) { return rank[a] > rank[b] || (rank[a] == rank[b] && a < b); };
                    std::partial_sort(order.begin(), order.begin() + top_, order.end(), higher);
                    for (vint i = 0; i < top_; ++i) {
                        outputWriter.setInt(0, graph.id_of(order[i]));
                        outputWriter.setFloat(1, rank[order[i]]);
                        outputWriter.next();
                    }
                    return;
                }
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
        parameterTypes.addInt("top");
        parameterTypes.addInt("threads");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GPagerank>(srvInterface.allocator); }
};

RegisterFactory(GPagerankFactory);
