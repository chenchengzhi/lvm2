/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "activate.h"
#include "display.h"
#include "log.h"
#include "fs.h"

static void _build_lv_name(char *buffer, size_t s, struct logical_volume *lv)
{
	snprintf(buffer, s, "%s_%s", lv->vg->name, lv->name);
}

static struct dm_task *_setup_task(struct logical_volume *lv, int task)
{
	char name[128];
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(task))) {
		stack;
		return NULL;
	}

	_build_lv_name(name, sizeof(name), lv);
	dm_task_set_name(dmt, name);

	return dmt;
}

int lv_info(struct logical_volume *lv, struct dm_info *info)
{
	int r = 0;
	struct dm_task *dmt;

	if (!(dmt = _setup_task(lv, DM_DEVICE_INFO))) {
		stack;
		return 0;
	}

	if (!dm_task_run(dmt)) {
		stack;
		goto out;
	}

	if (!dm_task_get_info(dmt, info)) {
		stack;
		goto out;
	}
	r = 1;

 out:
	dm_task_destroy(dmt);
	return r;
}

int lv_active(struct logical_volume *lv)
{
	int r = -1;
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return r;
	}

	return info.exists;
}

int lv_open_count(struct logical_volume *lv)
{
	int r = -1;
	struct dm_info info;

	if (!lv_info(lv, &info)) {
		stack;
		return r;
	}

	return info.open_count;
}

/*
 * Emit a target for a given segment.
 */
static int _emit_target(struct dm_task *dmt, struct stripe_segment *seg)
{
	char params[1024];
	uint64_t esize = seg->lv->vg->extent_size;
	uint32_t s, stripes = seg->stripes;
	int w, tw;

	for (w = 0, s = 0; s < stripes; s++, w += tw) {
		tw = snprintf(params + w, sizeof(params) - w,
			      "%s %" PRIu64 "%s",
			      dev_name(seg->area[s].pv->dev),
			      (seg->area[s].pv->pe_start +
			       (esize * seg->area[s].pe)),
			      s == (stripes - 1) ? "" : " ");

		if (tw < 0) {
			log_err("Insufficient space to write target "
				"parameters.");
			return 0;
		}
	}

	if (!dm_task_add_target(dmt, esize * seg->le, esize * seg->len,
				stripes == 1 ? "linear" : "striped",
				params)) {
		stack;
		return 0;
	}

	return 1;
}

int _load(struct logical_volume *lv, int task)
{
	int r = 0;
	struct dm_task *dmt;
	struct list *segh;
	struct stripe_segment *seg;

	if (!(dmt = _setup_task(lv, task))) {
		stack;
		return 0;
	}

	list_iterate(segh, &lv->segments) {
		seg = list_item(segh, struct stripe_segment);
		if (!_emit_target(dmt, seg)) {
			log_error("Unable to activate logical volume '%s'",
				lv->name);
			goto out;
		}
	}

	if (!(r = dm_task_run(dmt)))
		stack;

	log_verbose("Logical volume %s activated", lv->name);

 out:
	dm_task_destroy(dmt);
	return r;
}

/* FIXME: Always display error msg */
int lv_activate(struct logical_volume *lv)
{
	return _load(lv, DM_DEVICE_CREATE) && fs_add_lv(lv);
}

int _suspend(struct logical_volume *lv, int sus)
{
	int r;
	struct dm_task *dmt;
	int task = sus ? DM_DEVICE_SUSPEND : DM_DEVICE_RESUME;

	if (!(dmt = _setup_task(lv, task))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		log_err("Couldn't %s device '%s'", sus ? "suspend" : "resume",
			lv->name);

	dm_task_destroy(dmt);
	return r;
}

int lv_reactivate(struct logical_volume *lv)
{
	int r;
	if (!_suspend(lv, 1)) {
		stack;
		return 0;
	}

	r = _load(lv, DM_DEVICE_RELOAD);

	if (!_suspend(lv, 0)) {
		stack;
		return 0;
	}

	return r;
}

int lv_deactivate(struct logical_volume *lv)
{
	int r;
	struct dm_task *dmt;

	if (!(dmt = _setup_task(lv, DM_DEVICE_REMOVE))) {
		stack;
		return 0;
	}

	if (!(r = dm_task_run(dmt)))
		stack;

	dm_task_destroy(dmt);

	fs_del_lv(lv);

	return r;
}

int activate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += (!lv_active(lv) && lv_activate(lv));
	}

	return count;
}

int lv_update_write_access(struct logical_volume *lv)
{
	return 0;
}

int deactivate_lvs_in_vg(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += ((lv_active(lv) == 1) && lv_deactivate(lv));
	}

	return count;
}

int lvs_in_vg_activated(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += (lv_active(lv) == 1);
	}

	return count;
}

int lvs_in_vg_opened(struct volume_group *vg)
{
	struct list *lvh;
	struct logical_volume *lv;
	int count = 0;

	list_iterate(lvh, &vg->lvs) {
		lv = &(list_item(lvh, struct lv_list)->lv);

		count += (lv_open_count(lv) == 1);
	}

	return count;
}
