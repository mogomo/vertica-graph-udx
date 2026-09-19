// gversion() OVER(): library version, snapshot format version, build flags.
// Thin adapter. The values come from src/engine/version.h.
#include "Vertica.h"
#include "../engine/version.h"

#include <cstring>

using namespace Vertica;

class GVersion : public TransformFunction
{
    virtual void processPartition(ServerInterface &srvInterface,
                                  PartitionReader &inputReader,
                                  PartitionWriter &outputWriter)
    {
        try {
            outputWriter.getStringRef(0).copy(vgraph::LIBRARY_VERSION);
            outputWriter.setInt(1, vgraph::FORMAT_VERSION);
            outputWriter.getStringRef(2).copy(vgraph::BUILD_FLAGS);
            outputWriter.next();
        } catch (std::exception &e) {
            vt_report_error(0, "gversion: %s", e.what());
        }
    }
};

class GVersionFactory : public TransformFunctionFactory
{
    virtual void getPrototype(ServerInterface &srvInterface,
                              ColumnTypes &argTypes, ColumnTypes &returnType)
    {
        // No arguments.
        returnType.addVarchar();
        returnType.addInt();
        returnType.addVarchar();
    }

    virtual void getReturnType(ServerInterface &srvInterface,
                               const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        outputTypes.addVarchar(32, "library_version");
        outputTypes.addInt("format_version");
        outputTypes.addVarchar(256, "build_flags");
    }

    virtual TransformFunction *createTransformFunction(ServerInterface &srvInterface)
    { return vt_createFuncObject<GVersion>(srvInterface.allocator); }
};

RegisterFactory(GVersionFactory);
