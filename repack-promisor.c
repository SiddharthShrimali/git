#include "git-compat-util.h"
#include "repack.h"
#include "hex.h"
#include "pack.h"
#include "packfile.h"
#include "path.h"
#include "repository.h"
#include "run-command.h"
#include "oidset.h"
#include "date.h"
#include "promisor-remote.h"
#include "strbuf.h"

/*
 * Append the drop-log entries to the already-computed path.
 * Returns -1 on any I/O failure so the caller can warn once.
 * Keeping this in a separate helper avoids goto-based cleanup
 * in append_drop_log();
 */
static int write_to_drop_log(struct repository *repo,
			     const char *path,
			     const struct oidset *dropped,
			     const char *stamp,
			     const char *filter_spec,
			     const char *remotes)
{
	struct oidset_iter iter;
	const struct object_id *oid;
	FILE *fp;

	if (safe_create_leading_directories(repo, (char *)path)) {
		warning(_("could not create leading directories for '%s'"), path);
		return -1;
	}

	fp = fopen(path, "a");
	if (!fp) {
		warning_errno(_("could not open '%s'"), path);
		return -1;
	}

	oidset_iter_init(dropped, &iter);
	while ((oid = oidset_iter_next(&iter))) {
		if (fprintf(fp, "%s %s filter=%s remote=%s\n",
				oid_to_hex(oid), stamp,
				filter_spec ? filter_spec : "",
				remotes) < 0) {
			warning(_("could not write to '%s'"), path);
			fclose(fp);
			return -1;
		}
	}

	if (fclose(fp)) {
		warning_errno(_("could not close '%s'"), path);
		return -1;
	}

	return 0;
}

void append_drop_log(struct repository *repo,
		     const struct oidset *dropped,
		     const char *filter_spec)
{
	char *path;
	struct strbuf stamp = STRBUF_INIT;
	struct strbuf remotes = STRBUF_INIT;
	struct promisor_remote *pr;

	if (!oidset_size(dropped))
		return;

	datestamp(&stamp);

	/*
	 * NEEDSWORK: we temporarily record all configured promisor remotes rather
	 * than the specific one a given object is recoverable from because there
	 * is currently no way to determine that locally. it would require
	 * asking the remote whether it has the object. A "remote-object-info"
	 * command is being added to the "git cat-file --batch" protocol for
	 * this kind of query. Once it is merged in the codebase, this should
	 * record the exact promisor remote that has each dropped object.
	 */
	for (pr = repo_promisor_remote_find(repo, NULL); pr; pr = pr->next) {
		if (remotes.len)
			strbuf_addch(&remotes, ',');
		strbuf_addstr(&remotes, pr->name);
	}

	path = repo_git_path(repo, "objects/info/promisor-dropped");

	if (write_to_drop_log(repo, path, dropped, stamp.buf,
			filter_spec, remotes.buf))
		warning(_("could not record all dropped objects in the drop log"));

	strbuf_release(&stamp);
	strbuf_release(&remotes);
	free(path);
}

struct write_oid_context {
	struct child_process *cmd;
	const struct git_hash_algo *algop;
	const struct oidset *to_drop;
};

/*
 * Write oid to the given struct child_process's stdin, starting it first if
 * necessary.
 */
static int write_oid(const struct object_id *oid,
		     struct object_info *oi UNUSED,
		     void *data)
{
	struct write_oid_context *ctx = data;
	struct child_process *cmd = ctx->cmd;

	/*
	 * Objects in to_drop are being removed from the repository, so
	 * omit them from the rebuilt promisor pack. Each such object is a
	 * promisor object and therefore remains recoverable from the
	 * promisor remote.
	 */
	if (ctx->to_drop && oidset_contains(ctx->to_drop, oid))
		return 0;

	if (cmd->in == -1) {
		if (start_command(cmd))
			die(_("could not start pack-objects to repack promisor objects"));
	}

	if (write_in_full(cmd->in, oid_to_hex(oid), ctx->algop->hexsz) < 0 ||
	    write_in_full(cmd->in, "\n", 1) < 0)
		die(_("failed to feed promisor objects to pack-objects"));
	return 0;
}

static void finish_repacking_promisor_objects(struct repository *repo,
					      struct child_process *cmd,
					      struct string_list *names,
					      const char *packtmp)
{
	struct strbuf line = STRBUF_INIT;
	FILE *out;

	close(cmd->in);

	out = xfdopen(cmd->out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		struct string_list_item *item;
		char *promisor_name;

		if (line.len != repo->hash_algo->hexsz)
			die(_("repack: Expecting full hex object ID lines only from pack-objects."));
		item = string_list_append(names, line.buf);

		/*
		 * pack-objects creates the .pack and .idx files, but not the
		 * .promisor file. Create the .promisor file, which is empty.
		 *
		 * NEEDSWORK: fetch-pack sometimes generates non-empty
		 * .promisor files containing the ref names and associated
		 * hashes at the point of generation of the corresponding
		 * packfile, but this would not preserve their contents. Maybe
		 * concatenate the contents of all .promisor files instead of
		 * just creating a new empty file.
		 */
		promisor_name = mkpathdup("%s-%s.promisor", packtmp,
					  line.buf);
		write_promisor_file(promisor_name, NULL, 0);

		item->util = generated_pack_populate(item->string, packtmp);

		free(promisor_name);
	}

	fclose(out);
	if (finish_command(cmd))
		die(_("could not finish pack-objects to repack promisor objects"));
	strbuf_release(&line);
}

void repack_promisor_objects(struct repository *repo,
			     const struct pack_objects_args *args,
			     struct string_list *names, const char *packtmp,
			     const struct oidset *to_drop)
{
	struct write_oid_context ctx;
	struct child_process cmd = CHILD_PROCESS_INIT;

	prepare_pack_objects(&cmd, args, packtmp);
	cmd.in = -1;

	/*
	 * NEEDSWORK: Giving pack-objects only the OIDs without any ordering
	 * hints may result in suboptimal deltas in the resulting pack. See if
	 * the OIDs can be sent with fake paths such that pack-objects can use a
	 * {type -> existing pack order} ordering when computing deltas instead
	 * of a {type -> size} ordering, which may produce better deltas.
	 */
	ctx.cmd = &cmd;
	ctx.algop = repo->hash_algo;
	ctx.to_drop = to_drop;
	odb_for_each_object(repo->objects, NULL, write_oid, &ctx,
			    ODB_FOR_EACH_OBJECT_PROMISOR_ONLY);

	if (cmd.in == -1) {
		/* No packed objects; cmd was never started */
		child_process_clear(&cmd);
		return;
	}

	finish_repacking_promisor_objects(repo, &cmd, names, packtmp);
}

void pack_geometry_repack_promisors(struct repository *repo,
				    const struct pack_objects_args *args,
				    const struct pack_geometry *geometry,
				    struct string_list *names,
				    const char *packtmp)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	FILE *in;

	if (!geometry->promisor_split)
		return;

	prepare_pack_objects(&cmd, args, packtmp);
	strvec_push(&cmd.args, "--stdin-packs");
	cmd.in = -1;
	if (start_command(&cmd))
		die(_("could not start pack-objects to repack promisor packs"));

	in = xfdopen(cmd.in, "w");
	for (size_t i = 0; i < geometry->promisor_split; i++)
		fprintf(in, "%s\n", pack_basename(geometry->promisor_pack[i]));
	for (size_t i = geometry->promisor_split; i < geometry->promisor_pack_nr; i++)
		fprintf(in, "^%s\n", pack_basename(geometry->promisor_pack[i]));
	fclose(in);

	finish_repacking_promisor_objects(repo, &cmd, names, packtmp);
}
