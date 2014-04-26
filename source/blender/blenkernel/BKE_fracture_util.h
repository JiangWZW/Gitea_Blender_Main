#ifndef BKE_FRACTURE_UTIL_H
#define BKE_FRACTURE_UTIL_H

#include "DNA_fracture_types.h"

Shard *BKE_fracture_shard_boolean(Object *obj, DerivedMesh *dm_parent, Shard* child);
Shard *BKE_fracture_shard_bisect(struct BMesh *bm_parent, Shard* child, float obmat[4][4], bool use_fill);

#endif // BKE_FRACTURE_UTIL_H
