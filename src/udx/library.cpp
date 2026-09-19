// Library metadata shown in v_catalog.user_libraries.
#include "Vertica.h"
#include "BuildInfo.h"
#include "../engine/version.h"

RegisterLibrary("Mo (github.com/mogomo)",
                __DATE__,
                vgraph::LIBRARY_VERSION,
                VERTICA_BUILD_ID_Brand_Version,
                "https://github.com/mogomo/vertica-graph-udx",
                "vgraph: graph traversal on Vertica tables",
                "",
                "");
