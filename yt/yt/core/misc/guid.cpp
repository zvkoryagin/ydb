#include "guid.h"

#include <yt/yt/core/ytree/serialize.h>

namespace NYT {
    
////////////////////////////////////////////////////////////////////////////////

using NYTree::Serialize;

REGISTER_INTERMEDIATE_PROTO_INTEROP_REPRESENTATION(NProto::TGuid, TGuid)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
