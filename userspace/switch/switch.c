/*
 *    This file is part of LiSA Command Line Interface.
 *
 *    LiSA Command Line Interface is free software; you can redistribute it 
 *    and/or modify it under the terms of the GNU General Public License 
 *    as published by the Free Software Foundation; either version 2 of the 
 *    License, or (at your option) any later version.
 *
 *    LiSA Command Line Interface is distributed in the hope that it will be 
 *    useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with LiSA Command Line Interface; if not, write to the Free 
 *    Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, 
 *    MA  02111-1307  USA
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "mm.h"
#include "switch.h"
#include "sw_api.h"

#ifdef LiSA
#include "lisa.h"
#endif

#ifdef Linux
#include "linux.h"
#endif

/*
 * Switch shared memory structure
 */
struct switch_mem {
	/* Enable secrets (crypted) */
	struct {
		char secret[SW_SECRET_LEN + 1];
	} enable[SW_MAX_ENABLE+1];
	/* Line vty passwords (clear text) */
	struct {
		char passwd[SW_PASS_LEN + 1];
	} vty[SW_MAX_VTY + 1];
	/* CDP configuration */
	struct cdp_configuration cdp;
	/* RSTP configuration */
	struct rstp_configuration rstp;
	/* List of interface tags */
	struct mm_list_head if_tags;
	/* List of vlan descriptions */
	struct mm_list_head vlan_descs;
	/* List of interface descriptions */
	struct mm_list_head if_descs;
};

struct if_tag {
	int if_index;
	char tag[SW_MAX_TAG + 1];
	struct mm_list_head lh;
};

struct vlan_desc {
	int vlan_id;
	char desc[SW_MAX_VLAN_NAME + 1];
	struct mm_list_head lh;
};

struct if_desc {
	int if_index;
	char desc[SW_MAX_PORT_DESC + 1];
	struct mm_list_head lh;
};

static struct mm_private *mm = NULL;
#define SHM ((struct switch_mem *)MM_STATIC(mm))
struct switch_operations *sw_ops;

static mm_ptr_t __switch_get_if_tag(int if_index) {
	mm_ptr_t ret;

	mm_list_for_each(mm, ret, mm_ptr(mm, &SHM->if_tags)) {
		struct if_tag *tag =
			mm_addr(mm, mm_list_entry(ret, struct if_tag, lh));
		if (tag->if_index == if_index)
			return ret;
	}

	return MM_NULL;
}

static int __switch_del_if_tag(int if_index) {
	mm_ptr_t lh = __switch_get_if_tag(if_index);

	if (MM_NULL == lh) {
		errno = ENODEV;
		return -1;
	}

	mm_list_del(mm, lh);
	mm_free(mm, mm_list_entry(lh, struct if_tag, lh));
	return 0;
}

static mm_ptr_t __switch_get_tag_if(char *tag) {
	mm_ptr_t ret;

	mm_list_for_each(mm, ret, mm_ptr(mm, &SHM->if_tags)) {
		struct if_tag *s_tag =
			mm_addr(mm, mm_list_entry(ret, struct if_tag, lh));
		if (!strcmp(tag, s_tag->tag))
			return ret;
	}

	return MM_NULL;
}

static mm_ptr_t __switch_get_if_desc(int if_index)
{
	mm_ptr_t ret;

	mm_list_for_each(mm, ret, mm_ptr(mm, &SHM->if_descs)) {
		struct if_desc *desc =
			mm_addr(mm, mm_list_entry(ret, struct if_desc, lh));
		if(desc->if_index == if_index)
			return ret;
	}

	return MM_NULL;
}

static int __switch_del_if_desc(int if_index)
{
	mm_ptr_t lh = __switch_get_if_desc(if_index);

	if(MM_NULL == lh) {
		errno = ENODEV;
		return -1;
	}

	mm_list_del(mm, lh);
	mm_free(mm, mm_list_entry(lh, struct if_desc, lh));
	return 0;
}

static mm_ptr_t __switch_get_vlan_desc(int vlan_id) {
	mm_ptr_t ret;

	mm_list_for_each(mm, ret, mm_ptr(mm, &SHM->vlan_descs)) {
		struct vlan_desc *desc =
			mm_addr(mm, mm_list_entry(ret, struct vlan_desc, lh));
		if (desc->vlan_id == vlan_id)
			return ret;
	}

	return MM_NULL;
}

static int __switch_del_vlan_desc(int vlan_id) {
	mm_ptr_t lh = __switch_get_vlan_desc(vlan_id);

	if (MM_NULL == lh) {
		errno = ENODEV;
		return -1;
	}
	mm_list_del(mm, lh);
	mm_free(mm, mm_list_entry(lh, struct vlan_desc, lh));
	return 0;
}

static void switch_init_rstp(void)
{
	struct rstp_configuration rstp;
	//TODO get proper MAC address
	unsigned char addr[6] = {0x00,0xE0,0x81,0xB1,0xC0,0x38};
	unsigned short defaultPriority = 32768;

	rstp.enabled = 0;

	memcpy(&rstp.BridgeIdentifier.bridge_priority, &defaultPriority, 2);
	memcpy(&rstp.BridgeIdentifier.bridge_address, addr, 6);

	memset(&rstp.BridgePriority, 0, sizeof(struct priority_vector));
	memcpy(&rstp.BridgePriority.root_bridge_id, &rstp.BridgeIdentifier, sizeof(struct bridge_id));
	memcpy(&rstp.BridgePriority.designated_bridge_id, &rstp.BridgeIdentifier, sizeof(struct bridge_id));

	rstp.BridgeTimes.MessageAge = 0;
	rstp.BridgeTimes.MaxAge = 20;
	rstp.BridgeTimes.ForwardDelay = 15;
	rstp.BridgeTimes.HelloTime = 2;

	rstp.ForceProtocolVersion = 2;

	/* store initial config into the shared memory */
	switch_set_rstp(&rstp);
}

static void switch_init_cdp(void)
{
	struct cdp_configuration cdp;

	/* Initial default values for the cdp configuration */
	cdp.enabled  = 1;                /* CDP is enabled by default */
	cdp.version  = CDP_DFL_VERSION;  /* CDPv2*/
	cdp.holdtime = CDP_DFL_HOLDTIME; /* 180 seconds */
	cdp.timer    = CDP_DFL_TIMER;    /* 60 seconds */

	/* store initial config into the shared memory */
	switch_set_cdp(&cdp);
}

int switch_init(void) {
	int i;
	char desc[SW_MAX_VLAN_NAME + 1];

#ifdef LiSA
	sw_ops = &lisa_ctx.sw_ops;
#endif
	if (mm)
		return 0;
	mm = mm_create("lisa", sizeof(struct switch_mem), 4096);
	if (!mm)
		return  -1;

	if (!mm->init)
		return 0;

	memset(SHM, 0, sizeof(struct switch_mem));
	MM_INIT_LIST_HEAD(mm, mm_ptr(mm, &SHM->vlan_descs));
	MM_INIT_LIST_HEAD(mm, mm_ptr(mm, &SHM->if_tags));
	MM_INIT_LIST_HEAD(mm, mm_ptr(mm, &SHM->if_descs));
	switch_init_cdp();
	switch_init_rstp();

	if (sw_ops->backend_init(sw_ops))
		return -1;

	if (sw_ops->vlan_add(sw_ops, 1))
		return -1;
	if (switch_set_vlan_desc(1, "default"))
		return -1;

	for (i = 1002; i <= 1005; i++) {
		__default_vlan_name(desc, i);
		if (sw_ops->vlan_add(sw_ops, i))
			return -1;
		if (switch_set_vlan_desc(i, desc))
			return -1;
	}
	return 0;
}

int switch_auth(int type, int level,
		int (*auth)(char *pw, void *priv), void *priv)
{
	char *passwd;
	int err = EINVAL;

	if (!auth) {
		errno = err;
		return -1;
	}

	mm_lock(mm);
	switch (type) {
	case SHARED_AUTH_VTY:
		if (level < 0 || level > SW_MAX_VTY)
			goto out_unlock;
		passwd = SHM->vty[level].passwd;
		break;
	case SHARED_AUTH_ENABLE:
		if (level < 0 || level > SW_MAX_ENABLE)
			goto out_unlock;
		passwd = SHM->enable[level].secret;
		break;
	default:
		goto out_unlock;
		break;
	}
	err = auth(passwd, priv);

out_unlock:
	mm_unlock(mm);

	return err;
}

int switch_set_passwd(int type, int level, char *passwd)
{
	int err = EINVAL;

	if (!passwd) {
		errno = err;
		return -1;
	}

	mm_lock(mm);
	switch (type) {
	case SHARED_AUTH_VTY:
		if (level < 0 || level > SW_MAX_VTY)
			goto out_unlock;
		strncpy(SHM->vty[level].passwd, passwd, SW_PASS_LEN);
		SHM->vty[level].passwd[SW_PASS_LEN] = 0;
		err = 0;
		break;
	case SHARED_AUTH_ENABLE:
		if (level < 0 || level > SW_MAX_ENABLE)
			goto out_unlock;
		strncpy(SHM->enable[level].secret, passwd, SW_SECRET_LEN);
		SHM->enable[level].secret[SW_SECRET_LEN] = 0;
		err = 0;
		break;
	default:
		break;
	}

out_unlock:
	mm_unlock(mm);
	if (err) {
		errno = err;
		err = -1;
	}

	return err;
}

int switch_get_if_tag(int if_index, char *tag) {
	mm_ptr_t ptr;
	struct if_tag *s_tag;

	mm_lock(mm);
	ptr = __switch_get_if_tag(if_index);
	if (MM_NULL == ptr) {
		mm_unlock(mm);
		return 1;
	}
	s_tag = mm_addr(mm, mm_list_entry(ptr, struct if_tag, lh));
	strcpy(tag, s_tag->tag);

	mm_unlock(mm);
	return 0;
}


int switch_set_if_tag(int if_index, char *tag, int *other_if) {
	mm_ptr_t lh, mm_s_tag;
	struct if_tag *s_tag;
	int ret;

	mm_lock(mm);

	if (NULL == tag) {
		ret = __switch_del_if_tag(if_index);
		mm_unlock(mm);
		return ret;
	}

	lh = __switch_get_tag_if(tag);
	if (MM_NULL != lh) {
		if (*other_if) {
			s_tag = mm_addr(mm, mm_list_entry(lh, struct if_tag, lh));
			s_tag->if_index = *other_if;
		}
		mm_unlock(mm);
		return 1;
	}

	lh = __switch_get_if_tag(if_index);
	if (MM_NULL != lh) {
		s_tag = mm_addr(mm, mm_list_entry(lh, struct if_tag, lh));
		strncpy(s_tag->tag, tag, SW_MAX_TAG);
		s_tag->tag[SW_MAX_TAG] = '\0';
		mm_unlock(mm);
		return 0;
	}

	mm_s_tag = mm_alloc(mm, sizeof(struct if_tag));
	/* first save mm pointer obtained from mm_alloc, then compute s_tag
	 * pointer, because mm_alloc() can change mm->area if the shm area
	 * is extended (refer to README.mm for details) */
	s_tag = mm_addr(mm, mm_s_tag);
	if (NULL == s_tag) {
		if (*other_if)
			*other_if = 0;
		mm_unlock(mm);
		return 1;
	}

	s_tag->if_index = if_index;
	strncpy(s_tag->tag, tag, SW_MAX_TAG);
	s_tag->tag[SW_MAX_TAG] = '\0';
	mm_list_add(mm, mm_ptr(mm, &s_tag->lh), mm_ptr(mm, &SHM->if_tags));

	mm_unlock(mm);
	return 0;
}

int switch_get_vlan_desc(int vlan_id, char *desc)
{
	mm_ptr_t ptr;
	struct vlan_desc *s_desc;

	if (desc == NULL) {
		errno = EFAULT;
		return -1;
	}

	mm_lock(mm);
	ptr = __switch_get_vlan_desc(vlan_id);
	if (MM_NULL == ptr) {
		mm_unlock(mm);
		desc = NULL;
		errno = ENODEV;
		return -1;
	}
	s_desc = mm_addr(mm, mm_list_entry(ptr, struct vlan_desc, lh));
	strcpy(desc, s_desc->desc);
	desc[SW_MAX_VLAN_NAME] = '\0';

	mm_unlock(mm);

	return 0;
}

int switch_set_vlan_desc(int vlan_id, const char *desc)
{
	mm_ptr_t lh, mm_s_desc;
	struct vlan_desc *s_desc;
	int ret = 0;

	mm_lock(mm);

	if (NULL == desc) {
		/* Set description to default. */
		lh = __switch_get_vlan_desc(vlan_id);
		if (MM_NULL != lh) {
			char default_desc[SW_MAX_VLAN_NAME];
			__default_vlan_name(default_desc, vlan_id);
			s_desc = mm_addr(mm, mm_list_entry(lh, struct vlan_desc,
					lh));
			strncpy(s_desc->desc, default_desc, SW_MAX_VLAN_NAME);
			s_desc->desc[SW_MAX_VLAN_NAME] = '\0';
		}
		else {
			errno = ENODEV;
			ret = -1;
		}
		goto out_unlock;
	}
	lh = __switch_get_vlan_desc(vlan_id);
	if (MM_NULL != lh) {
		/* Vlan already has a description, so replace it. */
		s_desc = mm_addr(mm, mm_list_entry(lh, struct vlan_desc, lh));
		strncpy(s_desc->desc, desc, SW_MAX_VLAN_NAME);
		s_desc->desc[SW_MAX_VLAN_NAME] = '\0';
		ret = 0;
		goto out_unlock;
	}

	/* Create a new entry for vlan descriptions list. */
	mm_s_desc = mm_alloc(mm, sizeof(struct vlan_desc));
	/* first save mm pointer obtained from mm_alloc, then compute s_desc
	 * pointer, because mm_alloc() can change mm->area if the shm area
	 * is extended (refer to README.mm for details) */
	s_desc = mm_addr(mm, mm_s_desc);
	if (NULL == s_desc) {
		errno = ENOMEM;
		ret = -1;
		goto out_unlock;
	}

	s_desc->vlan_id = vlan_id;
	strncpy(s_desc->desc, desc, SW_MAX_VLAN_NAME);
	s_desc->desc[SW_MAX_VLAN_NAME] = '\0';
	mm_list_add(mm, mm_ptr(mm, &s_desc->lh), mm_ptr(mm, &SHM->vlan_descs));

out_unlock:
	mm_unlock(mm);
	return ret;
}

int switch_del_vlan(int vlan_id)
{
	int ret = 0;

	mm_lock(mm);
	ret = __switch_del_vlan_desc(vlan_id);
	mm_unlock(mm);

	return ret;
}

int switch_get_if_desc(int if_index, char *desc)
{
	mm_ptr_t ptr;
	struct if_desc *sif_desc;

	mm_lock(mm);
	ptr = __switch_get_if_desc(if_index);
	if(MM_NULL == ptr) {
		mm_unlock(mm);
		errno = EINVAL;
		return -1;
	}

	sif_desc = mm_addr(mm, mm_list_entry(ptr, struct if_desc, lh));
	strncpy(desc, sif_desc->desc, SW_MAX_PORT_DESC);
	desc[SW_MAX_PORT_DESC] = '\0';

	mm_unlock(mm);
	return 0;
}

int switch_set_if_desc(int if_index, char *desc)
{
	mm_ptr_t lh, mm_s_desc;
	struct if_desc *sif_desc;
	int ret = 0;

	mm_lock(mm);

	/* no description was given */
	if(NULL == desc) {
		/* Set description to default value */
		lh = __switch_get_if_desc(if_index);
		if(MM_NULL != lh) {
			char default_desc[SW_MAX_PORT_DESC];
			__default_iface_name(default_desc);
			sif_desc = mm_addr(mm, mm_list_entry(lh, struct if_desc,
						lh));
			strncpy(sif_desc->desc, default_desc, SW_MAX_PORT_DESC);
			sif_desc->desc[SW_MAX_PORT_DESC] = '\0';
		} else {
			errno = ENODEV;
			ret = -1;
		}
		goto out_unlock;
	}

	/* interface already exists */
	lh = __switch_get_if_desc(if_index);
	if(MM_NULL != lh) {
		sif_desc = mm_addr(mm, mm_list_entry(lh, struct if_desc, lh));
		strncpy(sif_desc->desc, desc, SW_MAX_PORT_DESC);
		sif_desc->desc[SW_MAX_PORT_DESC] = '\0';
		goto out_unlock;
	}

	/* interface doesn't exist in shm */
	mm_s_desc = mm_alloc(mm, sizeof(struct if_desc));
	/* first save mm pointer obtained from mm_alloc, then compute s_desc
	 * pointer, because mm_alloc() can change mm->area if the shm area
	 * is extended (refer to README.mm for details) */
	sif_desc = mm_addr(mm, mm_s_desc);
	if (NULL == sif_desc) {
		errno = ENOMEM;
		ret = -1;
		goto out_unlock;
	}

	sif_desc->if_index = if_index;
	strncpy(sif_desc->desc, desc, SW_MAX_PORT_DESC);
	sif_desc->desc[SW_MAX_PORT_DESC] = '\0';
	mm_list_add(mm, mm_ptr(mm, &sif_desc->lh), mm_ptr(mm, &SHM->if_descs));

out_unlock:
	mm_unlock(mm);
	return ret;
}


int switch_del_if(int if_index)
{
	int ret;

	mm_lock(mm);
	ret = __switch_del_if_desc(if_index);
	mm_unlock(mm);

	return ret;
}

void switch_set_cdp(struct cdp_configuration *cdp)
{
	assert(cdp);
	mm_lock(mm);
	memcpy(&SHM->cdp, cdp, sizeof(struct cdp_configuration));
	mm_unlock(mm);
}

void switch_get_cdp(struct cdp_configuration *cdp)
{
	assert(cdp);
	mm_lock(mm);
	memcpy(cdp, &SHM->cdp, sizeof(struct cdp_configuration));
	mm_unlock(mm);
}

void switch_set_rstp(struct rstp_configuration *rstp)
{
	assert(rstp);
	mm_lock(mm);
	memcpy(&SHM->rstp, rstp, sizeof(struct rstp_configuration));
	mm_unlock(mm);
}

void switch_get_rstp(struct rstp_configuration *rstp)
{
	assert(rstp);
	mm_lock(mm);
	memcpy(rstp, &SHM->rstp, sizeof(struct rstp_configuration));
	mm_unlock(mm);
}
