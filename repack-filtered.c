#include "git-compat-util.h"
#include "repack.h"
#include "repository.h"
#include "run-command.h"
#include "string-list.h"
#include "hex.h"
#include "packfile.h"

int write_filtered_pack(const struct write_pack_opts *opts,
			struct existing_packs *existing,
			struct string_list *names)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	FILE *in;
	int ret;
	const char *caret;
	const char *pack_prefix = write_pack_opts_pack_prefix(opts);

	prepare_pack_objects(&cmd, opts->po_args, opts->destination);

	strvec_push(&cmd.args, "--stdin-packs");

	for_each_string_list_item(item, &existing->kept_packs)
		strvec_pushf(&cmd.args, "--keep-pack=%s", item->string);

	cmd.in = -1;

	ret = start_command(&cmd);
	if (ret)
		return ret;

	/*
	 * Here 'names' contains only the pack(s) that were just
	 * written, which is exactly the packs we want to keep. Also
	 * 'existing_kept_packs' already contains the packs in
	 * 'keep_pack_list'.
	 */
	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, names)
		fprintf(in, "^%s-%s.pack\n", pack_prefix, item->string);
	for_each_string_list_item(item, &existing->non_kept_packs)
		fprintf(in, "%s.pack\n", item->string);
	for_each_string_list_item(item, &existing->cruft_packs)
		fprintf(in, "%s.pack\n", item->string);
	caret = opts->po_args->pack_kept_objects ? "" : "^";
	for_each_string_list_item(item, &existing->kept_packs)
		fprintf(in, "%s%s.pack\n", caret, item->string);
	fclose(in);

	return finish_pack_objects_cmd(existing->repo->hash_algo, opts, &cmd,
				       names);
}

int enumerate_filtered_objects(struct repository *repo,
				const char *packtmp,
				struct string_list *names,
				int dry_run)
{
	struct strbuf pack_path = STRBUF_INIT;
	struct packed_git *p = NULL;
	const char *hash;
	uint32_t i;
	int ret = 0;

	if (!names->nr)
		return 0; /* filter matched no objects - nothing to enumerate */

	/*
	 * write_filtered_pack() appends the new pack's hash as the
	 * last entry in names. The pack sits at <packtmp>-<hash>.pack
	 * before generated_pack_install() moves it to packdir.
	 */
	hash = names->items[names->nr - 1].string;

	/*
	 * add_packed_git() expects a path ending in ".idx" (it derives the
	 * other extensions internally), while unlink_pack_path() expects a
	 * path ending in ".pack". We build the ".idx" form first for the
	 * open call below, then swap the suffix to ".pack" in the cleanup
	 * block before calling unlink_pack_path().
	 */
	strbuf_addf(&pack_path, "%s-%s.idx", packtmp, hash);

	p = add_packed_git(repo, pack_path.buf, pack_path.len, 1);
	if (!p) {
	    ret = error(_("could not open filtered pack: %s"),
		pack_path.buf);
	    goto cleanup;
	}

	if (open_pack_index(p)) {
		ret = error(_("could not open index for filtered pack"));
		goto cleanup;
	}

	if (dry_run) {
		for (i = 0; i < p->num_objects; i++) {
			struct object_id oid;
			if (nth_packed_object_id(&oid, p, i) < 0) {
				ret = error(_("could not read object %u from filtered pack"), i);
				goto cleanup;
			}
			printf("%s\n", oid_to_hex(&oid));
		}
	}

cleanup:
	if (p) {
		close_pack(p);
		free(p);
	}

	/*
	 * The filtered pack has been read. remove its temporary files so
	 * it does not get installed by the install loop. Future deletion
	 * commits may need to defer this unlink until after verification.
	 *
	 * Swap the ".idx" suffix added above for ".pack", which is the form
	 * unlink_pack_path() expects (it strips ".pack" and re-appends each
	 * known extension to unlink them all).
	 */
	strbuf_setlen(&pack_path, pack_path.len - strlen(".idx"));
	strbuf_addstr(&pack_path, ".pack");
	unlink_pack_path(pack_path.buf, 1);
	strbuf_release(&pack_path);
	return ret;
}
