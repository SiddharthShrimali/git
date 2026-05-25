#define USE_THE_REPOSITORY_VARIABLE

#include "builtin.h"
#include "config.h"
#include "environment.h"
#include "parse-options.h"
#include "repository.h"
#include "string-list.h"
#include "revision.h"
#include "promisor-remote.h"
#include "path-walk.h"

/*
 * storing candidate blob that survived all three filters:
 *   1 - type = OBJ_BLOB
 *   2 - inflated size >= min_size
 *   3 - not referenced by the current index (default) or
 *	 not an already-promised object
 *
 * store the inflated size so that we can report total reclaimable
 * space without a second pass.
 */

struct blob_candidate {
	struct object_id oid;
	unsigned long size; /* storing the size of the blob to
				avoid parsing it later (inflated size) */
};

struct candidate_list {
	struct blob_candidate *candidates;
	size_t nr, alloc;
}; // a list of suitable candidates
