/**
* This file is part of rmlint.
*
*  rmlint is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  rmlint is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with rmlint.  If not, see <http://www.gnu.org/licenses/>.
*
* Authors:
*
*  - Christopher <sahib> Pahl 2010-2020 (https://github.com/sahib)
*  - Daniel <SeeSpotRun> T.   2014-2020 (https://github.com/SeeSpotRun)
*
* Hosted on http://github.com/sahib/rmlint
*/

#include "file.h"

#include <string.h>

#include "session.h"

static RmOff rm_file_start_seek(RmFile *file) {
    RmCfg *cfg = file->session->cfg;

    if(cfg->use_absolute_start_offset) {
        return cfg->skip_start_offset;
    } else {
        return cfg->skip_start_factor * file->actual_file_size;
    }
}

RmOff rm_file_end_seek(RmFile *file) {
    RmCfg *cfg = file->session->cfg;
    RmOff file_size = file->actual_file_size;

    if(cfg->use_absolute_end_offset) {
        return MIN(cfg->skip_end_offset, file_size);
    } else {
        return MIN(file_size, file_size * cfg->skip_end_factor);
    }
}

RmOff rm_file_clamped_size(RmFile *file) {
    RmOff start_seek = rm_file_start_seek(file);
    RmOff end_seek = rm_file_end_seek(file);
    return end_seek - MIN(start_seek, end_seek);
}



RmFile *rm_file_new(struct RmSession *session, const char *path, RmStat *statp,
                    RmLintType type, bool is_ppath, unsigned path_index, short depth,
                    RmNode *node) {
    RmCfg *cfg = session->cfg;
    RmFile *self = g_slice_new0(RmFile);
    self->session = session;
    self->actual_file_size = statp->st_size;

    if(type == RM_LINT_TYPE_DUPE_CANDIDATE || type == RM_LINT_TYPE_PART_OF_DIRECTORY) {
        /* Check if the actual slice the file will be > 0; we don't want empty files in
         * shredder */
        if(self->actual_file_size != 0 && rm_file_clamped_size(self) == 0) {
            g_slice_free(RmFile, self);
            return NULL;
        }
    }
    else {
        // report other types as zero-size
        // TODO: review this, doesn't seem sensible
        self->actual_file_size = 0;
    }


    if(!node) {
        node = rm_trie_insert(&cfg->file_trie, path, statp->st_dev, statp->st_ino);
    }
    self->node = node;

    self->depth = depth;
    self->n_children = 0;

    self->mtime = rm_sys_stat_mtime_float(statp);
    self->is_new = (self->mtime >= cfg->min_mtime);

    self->hash_offset = rm_file_start_seek(self);

    self->lint_type = type;
    self->is_prefd = is_ppath;
    self->is_original = false;
    self->is_symlink = false;
    self->path_index = path_index;
    self->outer_link_count = -1;
    self->phys_offset = (RmOff)-1;

    self->ref_count = 1;
    return self;
}

void rm_file_build_path(RmFile *file, char *buf) {
    g_assert(file);

    rm_trie_build_path(&file->session->cfg->file_trie, file->node, buf, PATH_MAX);
}

void rm_file_build_dir_path(RmFile *file, char *buf) {
    g_assert(file);
    rm_trie_build_path(&file->session->cfg->file_trie, file->node->parent, buf, PATH_MAX);
}

RmFile *rm_file_copy(RmFile *file) {
    g_assert(file);

    RmFile *copy = g_slice_new0(RmFile);
    memcpy(copy, file, sizeof(RmFile));

    /* Only reset/copy the complex fields */
    copy->digest = rm_digest_copy(file->digest);
    copy->ext_cksum = g_strdup(file->ext_cksum);

    copy->cluster = NULL;
    copy->hardlinks = NULL;
    copy->shred_group = NULL;
    copy->signal = NULL;
    copy->parent_dir = NULL;
    copy->n_children = 0;
    copy->ref_count = 1;

    return copy;
}

static gint rm_file_unref_impl(RmFile *file, gboolean unref_hardlinks) {
    if(!file) {
        return 0;
    }
    if(!g_atomic_int_dec_and_test(&file->ref_count)) {
        // somebody still loves me!
        return 0;
    }

    gint freed = 0;
    if(file->hardlinks) {
        g_queue_remove(file->hardlinks, file);
        if(unref_hardlinks) {
            freed += rm_util_queue_foreach_remove(file->hardlinks,
                        (RmRFunc)rm_file_unref_impl, GINT_TO_POINTER(FALSE));
        }
        else if(file->hardlinks->length==0) {
            g_queue_free(file->hardlinks);
        }
    }

    if(file->reflink_count) {
        guint refs = --(*file->reflink_count);
        if(!refs) {
            g_free(file->reflink_count);
        }
    }

    if(file->ext_cksum) {
        g_free((char *)file->ext_cksum);
    }

    if(file->digest) {
        rm_digest_unref(file->digest);
    }
    g_slice_free(RmFile, file);
    return 1 + freed;
}

gint rm_file_unref(RmFile *file) {
    return rm_file_unref_impl(file, FALSE);
}

gint rm_file_unref_full(RmFile *file) {
    return rm_file_unref_impl(file, TRUE);
}

RmFile *rm_file_ref(RmFile *file) {
    g_atomic_int_inc (&file->ref_count);
    return file;
}


// TODO: this is replicated in formats/pretty.c...
static const char *LINT_TYPES[] = {
    [RM_LINT_TYPE_UNKNOWN] = "",
    [RM_LINT_TYPE_EMPTY_DIR] = "emptydir",
    [RM_LINT_TYPE_NONSTRIPPED] = "nonstripped",
    [RM_LINT_TYPE_BADLINK] = "badlink",
    [RM_LINT_TYPE_BADUID] = "baduid",
    [RM_LINT_TYPE_BADGID] = "badgid",
    [RM_LINT_TYPE_BADUGID] = "badugid",
    [RM_LINT_TYPE_EMPTY_FILE] = "emptyfile",
    [RM_LINT_TYPE_DUPE_CANDIDATE] = "duplicate_file_candidate",
    [RM_LINT_TYPE_DUPE_DIR_CANDIDATE] = "duplicate_dir_candidate",
    [RM_LINT_TYPE_DUPE] = "duplicate_file",
    [RM_LINT_TYPE_DUPE_DIR] = "duplicate_dir",
    [RM_LINT_TYPE_PART_OF_DIRECTORY] = "part_of_directory",
    [RM_LINT_TYPE_UNIQUE_FILE] = "unique_file"};

const char *rm_file_lint_type_to_string(RmLintType type) {
    return LINT_TYPES[MIN(type, sizeof(LINT_TYPES) / sizeof(const char *))];
}

RmLintType rm_file_string_to_lint_type(const char *type) {
    const int N = sizeof(LINT_TYPES) / sizeof(const char *);
    for(int i = 0; i < N; ++i) {
        if(g_strcmp0(type, LINT_TYPES[i]) == 0) {
            return (RmLintType)i;
        }
    }

    return RM_LINT_TYPE_UNKNOWN;
}

void rm_file_hardlink_add(RmFile *head, RmFile *link) {
    if(!head->hardlinks) {
        head->hardlinks = g_queue_new();
        g_queue_push_tail(head->hardlinks, head);
    }
    if(link != head) {
        link->hardlinks = head->hardlinks;
        g_queue_push_tail(head->hardlinks, link);
    }
}

void rm_file_reflink_add(RmFile *head, RmFile *link) {
    if(!head->reflink_count) {
        head->reflink_count = g_malloc0(sizeof(guint));
        *head->reflink_count = 1;
    }
    if(link != head) {
        link->reflink_count = head->reflink_count;
        ++(*head->reflink_count);
    }
}

static gint rm_file_foreach_hardlink(RmFile *f, RmRFunc func, gpointer user_data) {
    if(!f->hardlinks) {
        return func(f, user_data);
    }

    gint result = 0;
    for(GList *iter = f->hardlinks->head; iter; iter = iter->next) {
        result += func(iter->data, user_data);
    }
    return result;
}

void rm_file_cluster_add(RmFile *host, RmFile *guest) {
    g_assert(host);
    g_assert(guest);
    g_assert(!guest->cluster || host == guest);
    if(!host->cluster) {
        host->cluster = g_queue_new();
        if(guest != host) {
            rm_file_cluster_add(host, host);
        }
    }
    guest->cluster = host->cluster;
    g_queue_push_tail(host->cluster, guest);
}

void rm_file_cluster_remove(RmFile *file) {
    g_assert(file);
    g_assert(file->cluster);

    g_queue_remove(file->cluster, file);
    if(file->cluster->length == 0) {
        g_queue_free(file->cluster);
    }
    file->cluster = NULL;
}

gint rm_file_foreach(RmFile *f, RmRFunc func, gpointer user_data) {
    if(!f->cluster) {
        return rm_file_foreach_hardlink(f, func, user_data);
    }

    gint result = 0;
    for(GList *iter = f->cluster->head; iter; iter = iter->next) {
        result += rm_file_foreach_hardlink(iter->data, func, user_data);
    }
    return result;
}

enum RmFileCountType {
    RM_FILE_COUNT_FILES,
    RM_FILE_COUNT_PREFD,
    RM_FILE_COUNT_NPREFD,
    RM_FILE_COUNT_NEW,
};

static gint rm_file_count(RmFile *file, gint type) {
    switch(type) {
    case RM_FILE_COUNT_FILES:
        return 1;
    case RM_FILE_COUNT_PREFD:
        return file->is_prefd;
    case RM_FILE_COUNT_NPREFD:
        return !file->is_prefd;
    case RM_FILE_COUNT_NEW:
        return file->is_new;
    default:
        g_assert_not_reached();
        return 0;
    }
}

gint rm_file_n_files(RmFile *file) {
    return rm_file_foreach(file, (RmRFunc)rm_file_count,
                           GINT_TO_POINTER(RM_FILE_COUNT_FILES));
}

gint rm_file_n_new(RmFile *file) {
    return rm_file_foreach(file, (RmRFunc)rm_file_count,
                           GINT_TO_POINTER(RM_FILE_COUNT_NEW));
}

gint rm_file_n_prefd(RmFile *file) {
    return rm_file_foreach(file, (RmRFunc)rm_file_count,
                           GINT_TO_POINTER(RM_FILE_COUNT_PREFD));
}

gint rm_file_n_nprefd(RmFile *file) {
    return rm_file_foreach(file, (RmRFunc)rm_file_count,
                           GINT_TO_POINTER(RM_FILE_COUNT_NPREFD));
}
