
#include <minix/ds.h>
#include <string.h>

#include "syslib.h"

static message m;

PRIVATE int do_invoke_ds(int type, const char *ds_name)
{
	cp_grant_id_t g_key;
	size_t len_key;
	int access, r;

	if(type == DS_CHECK || type == DS_RETRIEVE_LABEL) {
		len_key = DS_MAX_KEYLEN;
		access = CPF_WRITE;
	} else {
		len_key = strlen(ds_name)+1;
		access = CPF_READ;
	}

	/* Grant for key. */
	g_key = cpf_grant_direct(DS_PROC_NR, (vir_bytes) ds_name,
		len_key, access);
	if(!GRANT_VALID(g_key)) 
		return errno;

	m.DS_KEY_GRANT = g_key;
	m.DS_KEY_LEN = len_key;

	r = _taskcall(DS_PROC_NR, type, &m);

	cpf_revoke(g_key);
	return r;
}

int ds_publish_label(const char *ds_name, u32_t value, int flags)
{
	m.DS_VAL = value;
	m.DS_FLAGS = DSF_TYPE_LABEL | flags;
	return do_invoke_ds(DS_PUBLISH, ds_name);
}

int ds_publish_u32(const char *ds_name, u32_t value, int flags)
{
	m.DS_VAL = value;
	m.DS_FLAGS = DSF_TYPE_U32 | flags;
	return do_invoke_ds(DS_PUBLISH, ds_name);
}

int ds_publish_str(const char *ds_name, char *value, int flags)
{
	if(strlen(value) >= DS_MAX_STRLEN)
		return EINVAL;
	strcpy((char *)(&m.DS_STRING), value);
	m.DS_FLAGS = DSF_TYPE_STR | flags;
	return do_invoke_ds(DS_PUBLISH, ds_name);
}

int ds_publish_mem(const char *ds_name, void *vaddr, size_t length, int flags)
{
	cp_grant_id_t gid;
	int r;

	/* Grant for memory range. */
	gid = cpf_grant_direct(DS_PROC_NR, (vir_bytes)vaddr, length, CPF_READ);
	if(!GRANT_VALID(gid))
		return errno;

	m.DS_VAL = gid;
	m.DS_VAL_LEN = length;
	m.DS_FLAGS = DSF_TYPE_MEM | flags;

	r = do_invoke_ds(DS_PUBLISH, ds_name);
	cpf_revoke(gid);

	return r;
}

int ds_publish_map(const char *ds_name, void *vaddr, size_t length, int flags)
{
	cp_grant_id_t gid;
	int r;

	if(((vir_bytes)vaddr % CLICK_SIZE != 0) || (length % CLICK_SIZE != 0))
		return EINVAL;

	/* Grant for mapped memory range. */
	gid = cpf_grant_direct(DS_PROC_NR, (vir_bytes)vaddr, length,
			CPF_READ | CPF_MAP);
	if(!GRANT_VALID(gid))
		return errno;

	m.DS_VAL = gid;
	m.DS_VAL_LEN = length;
	m.DS_FLAGS = DSF_TYPE_MAP | flags;

	r = do_invoke_ds(DS_PUBLISH, ds_name);
	cpf_revoke(gid);

	return r;
}

int ds_snapshot_map(const char *ds_name, int *nr_snapshot)
{
	int r;
	r = do_invoke_ds(DS_SNAPSHOT, ds_name);
	*nr_snapshot = m.DS_NR_SNAPSHOT;
	return r;
}

int ds_retrieve_label_name(char *ds_name, u32_t num)
{
	int r;
	m.DS_VAL = num;
	r = do_invoke_ds(DS_RETRIEVE_LABEL, ds_name);
	return r;
}

int ds_retrieve_label_num(const char *ds_name, u32_t *value)
{
	int r;
	m.DS_FLAGS = DSF_TYPE_LABEL;
	r = do_invoke_ds(DS_RETRIEVE, ds_name);
	*value = m.DS_VAL;
	return r;
}

int ds_retrieve_u32(const char *ds_name, u32_t *value)
{
	int r;
	m.DS_FLAGS = DSF_TYPE_U32;
	r = do_invoke_ds(DS_RETRIEVE, ds_name);
	*value = m.DS_VAL;
	return r;
}

int ds_retrieve_str(const char *ds_name, char *value, size_t len_str)
{
	int r;
	m.DS_FLAGS = DSF_TYPE_STR;
	r = do_invoke_ds(DS_RETRIEVE, ds_name);
	strncpy(value, (char *)(&m.DS_STRING), DS_MAX_STRLEN);
	value[DS_MAX_STRLEN - 1] = '\0';
	return r;
}

int ds_retrieve_mem(const char *ds_name, char *vaddr, size_t *length)
{
	cp_grant_id_t gid;
	int r;

	/* Grant for memory range. */
	gid = cpf_grant_direct(DS_PROC_NR, (vir_bytes)vaddr, *length, CPF_WRITE);
	if(!GRANT_VALID(gid))
		return errno;

	m.DS_VAL = gid;
	m.DS_VAL_LEN = *length;
	m.DS_FLAGS = DSF_TYPE_MEM;
	r = do_invoke_ds(DS_RETRIEVE, ds_name);
	*length = m.DS_VAL_LEN;
	cpf_revoke(gid);

	return r;
}

int ds_retrieve_map(const char *ds_name, char *vaddr, size_t *length,
		int nr_snapshot, int flags)
{
	cp_grant_id_t gid;
	int r;

	/* Map a mapped memory range. */
	if(flags & DSMF_MAP_MAPPED) {
		/* Request DS to grant. */
		m.DS_FLAGS = DSF_TYPE_MAP | DSMF_MAP_MAPPED;
		r = do_invoke_ds(DS_RETRIEVE, ds_name);
		if(r != OK)
			return r;

		/* Do the safemap. */
		if(*length > m.DS_VAL_LEN)
			*length = m.DS_VAL_LEN;
		*length = (size_t) CLICK_FLOOR(*length);
		r = sys_safemap(DS_PROC_NR, m.DS_VAL, 0,
				(vir_bytes)vaddr, *length, D, 0);

	/* Copy mapped memory range or a snapshot. */
	} else if(flags & (DSMF_COPY_MAPPED|DSMF_COPY_SNAPSHOT)) {
		/* Grant for memory range first. */
		gid = cpf_grant_direct(DS_PROC_NR, (vir_bytes)vaddr,
				*length, CPF_WRITE);
		if(!GRANT_VALID(gid))
			return errno;

		m.DS_VAL = gid;
		m.DS_VAL_LEN = *length;
		if(flags & DSMF_COPY_MAPPED) {
			m.DS_FLAGS = DSF_TYPE_MAP | DSMF_COPY_MAPPED;
		}
		else {
			m.DS_NR_SNAPSHOT = nr_snapshot;
			m.DS_FLAGS = DSF_TYPE_MAP | DSMF_COPY_SNAPSHOT;
		}
		r = do_invoke_ds(DS_RETRIEVE, ds_name);
		*length = m.DS_VAL_LEN;
		cpf_revoke(gid);
	}
	else {
		return EINVAL;
	}

	return r;
}

int ds_delete_u32(const char *ds_name)
{
	m.DS_FLAGS = DSF_TYPE_U32;
	return do_invoke_ds(DS_DELETE, ds_name);
}

int ds_delete_str(const char *ds_name)
{
	m.DS_FLAGS = DSF_TYPE_STR;
	return do_invoke_ds(DS_DELETE, ds_name);
}

int ds_delete_mem(const char *ds_name)
{
	m.DS_FLAGS = DSF_TYPE_MEM;
	return do_invoke_ds(DS_DELETE, ds_name);
}

int ds_delete_map(const char *ds_name)
{
	m.DS_FLAGS = DSF_TYPE_MAP;
	return do_invoke_ds(DS_DELETE, ds_name);
}

int ds_delete_label(const char *ds_name)
{
	m.DS_FLAGS = DSF_TYPE_LABEL;
	return do_invoke_ds(DS_DELETE, ds_name);
}

int ds_subscribe(const char *regexp, int flags)
{
	m.DS_FLAGS = flags;
	return do_invoke_ds(DS_SUBSCRIBE, regexp);
}

int ds_check(char *ds_key, int *type)
{
	int r;
	r = do_invoke_ds(DS_CHECK, ds_key);
	*type = m.DS_FLAGS;
	return r;
}
