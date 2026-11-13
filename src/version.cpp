// version.cpp: the build's stamp (version.h).

#include "version.h"

Build_Stamp build_stamp() {
    // The defines are C string literals (RIG_COMMIT_DIRTY a 0 or 1): the
    // build scripts write -DRIG_VERSION=\"0.1.0\" and so on.
    return {str(RIG_VERSION), str(RIG_COMMIT), RIG_COMMIT_DIRTY != 0, str(RIG_COMMIT_DATE), str(RIG_BUILD)};
}
