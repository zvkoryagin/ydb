#include "traceid.h"
#include <util/random/random.h>
#include <util/stream/str.h>

namespace NKikimr {
namespace NTracing {

TTraceID::TTraceID()
    : CreationTime(0)
    , RandomID(0)
{}

TTraceID::TTraceID(ui64 randomId, ui64 creationTime)
    : CreationTime(creationTime)
    , RandomID(randomId)
{}

TTraceID TTraceID::GenerateNew() {
    return TTraceID(RandomNumber<ui64>(), TInstant::Now().MicroSeconds());
}

TString TTraceID::ToString() const {
    TString result;
    TStringOutput out(result);
    Out(out);
    return result;
}

void TTraceID::Out(IOutputStream &o) const {
    char buf[240];
    sprintf(buf, "[ID:%" PRIu64 ", Created: %s]", RandomID, TInstant::MicroSeconds(CreationTime).ToRfc822StringLocal().data());
    o << buf;
}

bool TTraceID::operator<(const TTraceID &x) const {
    return CreationTime != x.CreationTime ? CreationTime < x.CreationTime : RandomID < x.RandomID;
}

bool TTraceID::operator>(const TTraceID &x) const {
    return (x < *this);
}

bool TTraceID::operator<=(const TTraceID &x) const {
    return CreationTime != x.CreationTime ? CreationTime < x.CreationTime : RandomID <= x.RandomID;
}

bool TTraceID::operator>=(const TTraceID &x) const {
    return (x <= *this);
}

bool TTraceID::operator==(const TTraceID &x) const {
    return CreationTime == x.CreationTime && RandomID == x.RandomID;
}

bool TTraceID::operator!=(const TTraceID &x) const {
    return CreationTime != x.CreationTime || RandomID != x.RandomID;
}

void TraceIDFromTraceID(const TTraceID& src, NKikimrTracing::TTraceID* dest) {
    Y_VERIFY_DEBUG(dest);
    dest->SetCreationTime(src.CreationTime);
    dest->SetRandomID(src.RandomID);
}

TTraceID TraceIDFromTraceID(const NKikimrTracing::TTraceID &proto) {
    return TTraceID(proto.GetRandomID(), proto.GetCreationTime());
}

}
}