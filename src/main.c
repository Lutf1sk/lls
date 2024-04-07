#include <lt/io.h>
#include <lt/arg.h>
#include <lt/darr.h>
#include <lt/mem.h>
#include <lt/debug.h>
#include <lt/str.h>
#include <lt/ansi.h>
#include <lt/math.h>
#include <lt/term.h>
#include <lt/sort.h>
#include <lt/ctype.h>

#define FORMAT_DEFAULT			0
#define FORMAT_DETAILED_LIST	1

#define SHOW_DEFAULT	0x00
#define SHOW_OWNER		0x01
#define SHOW_PERMIT		0x02
#define SHOW_SIZE		0x04
#define SHOW_HIDDEN		0x08

#ifdef LT_LINUX
#	include <sys/stat.h>
#	include <pwd.h>
#	include <grp.h>
#endif

typedef
struct entry {
	lstr_t name;
	lt_dirent_type_t type;
	lt_file_perms_t permit;
} entry_t;

b8 lstr_lesser_alphabetic(lstr_t a, lstr_t b) {
	usz len = lt_min_usz(a.len, b.len);
	for (usz i = 0; i < len; ++i) {
		u8 ac = lt_to_upper(a.str[i]), bc = lt_to_upper(b.str[i]);

		if (ac < bc) {
			return 1;
		}
		if (ac > bc) {
			return 0;
		}
	}
	return 0;
}

LT_DEFINE_QUICKSORT_FUNC(lstr_t, sort_list, lstr_lesser_alphabetic)

u32 format = FORMAT_DEFAULT;
u32 show = SHOW_DEFAULT;

b8 use_color = 0;

usz max_name_len = 1;
lt_darr(lstr_t) entries;

#define MAX_COLUMNS 64

void print_detailed(lstr_t path) {
	for (usz i = 0; i < lt_darr_count(entries); ++i) {
		lstr_t full_path = lt_lsbuild(lt_libc_heap, "%S/%S%c", path, entries[i], 0);
		--full_path.len;

		char* owner = "";
		char* group = "";
		char* color = "";

#ifdef LT_LINUX
		char permit[9];
		char size[8];

		lt_mset8(permit, '-', sizeof(permit));
		lt_mset8(size, ' ', sizeof(size));

		struct stat st;
		if (lstat(full_path.str, &st) < 0) {
			lt_werrf("failed to stat '%S': %S\n", full_path, lt_err_str(lt_errno()));
			lt_mfree(lt_libc_heap, full_path.str);
			continue;
		}

		if (st.st_mode & S_IRUSR) permit[0] = 'r';
		if (st.st_mode & S_IWUSR) permit[1] = 'w';
		if (st.st_mode & S_IXUSR) permit[2] = 'x';
		if (st.st_mode & S_IRGRP) permit[3] = 'r';
		if (st.st_mode & S_IWGRP) permit[4] = 'w';
		if (st.st_mode & S_IXGRP) permit[5] = 'x';
		if (st.st_mode & S_IROTH) permit[6] = 'r';
		if (st.st_mode & S_IWOTH) permit[7] = 'w';
		if (st.st_mode & S_IXOTH) permit[8] = 'x';

		lt_sprintf(size, "%mz", (usz)st.st_size);

		struct passwd* pwd = getpwuid(st.st_uid);
		if (pwd) {
			owner = pwd->pw_name;
		}
		struct group* grp = getgrgid(st.st_gid);
		if (grp) {
			group = grp->gr_name;
		}

		switch (lt_enttype_from_unix(st.st_mode)) {
		case LT_DIRENT_UNKNOWN: break;
		case LT_DIRENT_FILE:
			if (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) {
				color = LT_BOLD LT_FG_GREEN;
			}
			break;
		case LT_DIRENT_PIPE: color = LT_BOLD LT_FG_YELLOW; break;
		case LT_DIRENT_SOCKET: color = LT_BOLD LT_FG_MAGENTA; break;
		case LT_DIRENT_DEVICE: color = LT_BOLD LT_FG_YELLOW; break;
		case LT_DIRENT_DIR: color = LT_BOLD LT_FG_BLUE; break;
		case LT_DIRENT_SYMLINK: color = LT_BOLD LT_FG_CYAN; break;
		}
#else

#endif

		usz padusr = lt_max_isz(8 - strlen(owner), 1);
		usz padgrp = lt_max_isz(8 - strlen(group), 1);

		lt_printf("%S %s%r %s%r %S ", LSTR(permit, sizeof(permit)), owner, padusr, group, padgrp, LSTR(size, sizeof(size)));

		if (use_color) {
			lt_printf("%s%S"LT_RESET"\n", color, entries[i]);
		}
		else {
			lt_printf("%S\n", entries[i]);
		}
	}
}

void print_default(lstr_t path) {
	lt_err_t err;

	lt_term_init(0);
	lt_update_term_dimensions();
	usz term_w = lt_term_width;
	lt_term_restore();

	if (show & SHOW_PERMIT) {
		max_name_len += 4;
	}
	if (show & SHOW_SIZE) {
		max_name_len += 8;
	}

	lt_stat_t* entry_stats = lt_malloc(lt_libc_heap, lt_darr_count(entries) * sizeof(lt_stat_t));

	lt_printf("%uz / %uz\n", term_w, max_name_len);

	usz columns = lt_clamp_usz(term_w / max_name_len, 1, MAX_COLUMNS);

	u32 column_widths[MAX_COLUMNS];
	lt_mset32(column_widths, 1, sizeof(column_widths));
	u32 column_size_pad[MAX_COLUMNS];
	lt_mset32(column_size_pad, 1, sizeof(column_size_pad));

	for (usz i = 0; i < lt_darr_count(entries); ++i) {
		lstr_t full_path = lt_lsbuild(lt_libc_heap, "%S/%S", path, entries[i], 0);
		if ((err = lt_lstatp(full_path, &entry_stats[i]))) {
			lt_werrf("failed to stat '%S': %S\n", full_path, lt_err_str(err));
			lt_mfree(lt_libc_heap, full_path.str);
			lt_mzero(&entry_stats[i], sizeof(lt_stat_t));
			continue;
		}
		lt_mfree(lt_libc_heap, full_path.str);

		usz col = i % columns;
		isz size_len = lt_io_printf(lt_io_dummy_callb, NULL, "%mz", entry_stats[i].size);
		if (size_len) {
			column_size_pad[col] = lt_max_usz(column_size_pad[col], size_len + 1);
		}
		column_widths[col] = lt_max_usz(column_widths[col], entries[i].len + 2);
	}

	for (usz i = 0; i < lt_darr_count(entries); ) {
		usz row_end = lt_min_usz(i + columns, lt_darr_count(entries));

		for (; i < row_end; ++i) {
			usz col = i % columns;

			lstr_t name = entries[i];
			usz pad = column_widths[col] - name.len;

			if (!use_color) {
				lt_printf("%S%r ", name, pad);
				continue;
			}

			char* fg = "";
			char* bg = "";

			lt_stat_t stat = entry_stats[i];

			switch (stat.type) {
			case LT_DIRENT_UNKNOWN: break;
			case LT_DIRENT_FILE:
				if (stat.permit & LT_FILE_PERMIT_X) {
					fg = LT_BOLD LT_FG_GREEN;
				}
				break;
			case LT_DIRENT_PIPE: fg = LT_BOLD LT_FG_YELLOW; break;
			case LT_DIRENT_SOCKET: fg = LT_BOLD LT_FG_MAGENTA; break;
			case LT_DIRENT_DEVICE: fg = LT_BOLD LT_FG_YELLOW; break;
			case LT_DIRENT_DIR: fg = LT_BOLD LT_FG_BLUE; break;
			case LT_DIRENT_SYMLINK: fg = LT_BOLD LT_FG_CYAN; break;
			}


			if (show & SHOW_SIZE) {
				isz size_len = lt_printf("%mz", stat.size);
				if (size_len > 0) {
					lt_printf("%r ", column_size_pad[col] - size_len);
				}
			}
			if (show & SHOW_PERMIT) {
				char* r = (stat.permit & LT_FILE_PERMIT_R) ? "r" : "-";
				char* w = (stat.permit & LT_FILE_PERMIT_W) ? "w" : "-";
				char* x = "-";
				if (stat.permit & LT_FILE_PERMIT_X) {
					x = (stat.type == LT_DIRENT_DIR) ? "s" : "x";
				}

				lt_printf("%s%s%s ", r, w, x);
			}
			lt_printf("%s%s%S%r " LT_RESET, fg, bg, name, pad);
		}

		lt_printf("\n");
	}
}

int main(int argc, char** argv) {
	LT_DEBUG_INIT();

	char* cpath = NULL;

	lt_foreach_arg(arg, argc, argv) {
		if (lt_arg_flag(arg, 'h', CLSTR("help"))) {
			lt_printf(
				"usage: lls [OPTIONS] [PATH]\n"
				"options:\n"
				"  -a, --hidden  Show hidden entries.\n"
				"  -c, --color   Display output in multiple colors.\n"
				"  -h, --help    Display this information.\n"
				"  -l, --list    Show detailed list.\n"
				"  -o, --owner   Show entry owner.\n"
				"  -p, --permit  Show entry permissions.\n"
				"  -s, --size    Show file size\n"
			);
		}

		if (lt_arg_flag(arg, 'a', CLSTR("hidden"))) {
			show |= SHOW_HIDDEN;
			continue;
		}

		if (lt_arg_flag(arg, 'c', CLSTR("color"))) {
			use_color = 1;
			continue;
		}

		if (lt_arg_flag(arg, 'l', CLSTR("list"))) {
			format = FORMAT_DETAILED_LIST;
			continue;
		}

		if (lt_arg_flag(arg, 'o', CLSTR("owner"))) {
			show |= SHOW_OWNER;
			continue;
		}

		if (lt_arg_flag(arg, 'p', CLSTR("permit"))) {
			show |= SHOW_PERMIT;
			continue;
		}

		if (lt_arg_flag(arg, 's', CLSTR("size"))) {
			show |= SHOW_SIZE;
			continue;
		}

		if (cpath) {
			lt_ferrf("too many input paths\n");
		}
		cpath = *arg->it;
	}

	lstr_t path = cpath ? lt_lsfroms(cpath) : CLSTR("./");

	lt_err_t err;

	lt_stat_t stat;
	if ((err = lt_statp(path, &stat))) {
		lt_ferrf("failed to stat '%S': %S\n", path, lt_err_str(err));
	}

	if (stat.type == LT_DIRENT_FILE) {
		lt_file_t* fp = lt_fopenp(path, LT_FILE_R, 0, lt_libc_heap);
		if (!fp) {
			lt_ferrf("failed to open '%S': %S\n", path, lt_err_str(lt_errno()));
		}

#define BUFSZ 8192
		char buf[BUFSZ];

		isz res;
		while ((res = lt_fread(fp, buf, BUFSZ))) {
			if (res < 0) {
				lt_ferrf("failed to read from '%S': %S\n", path, lt_err_str(-res));
			}
			lt_printf("%S\n", LSTR(buf, res));
		}

		return 0;
	}

	if (stat.type != LT_DIRENT_DIR) {
		lt_ferrf("unhandled entry type for '%S'\n", path);
	}

	lt_dir_t* dir = lt_dopenp(path, lt_libc_heap);
	if (!dir) {
		lt_ferrf("failed to open '%S': %S\n", path, lt_err_str(lt_errno()));
	}

	entries = lt_darr_create(lstr_t, 1024, lt_libc_heap);
	LT_ASSERT(entries != NULL);

	lt_foreach_dirent(ent, dir) {
		usz full_path_len = ent->name.len + path.len + 1;
		if (full_path_len > LT_PATH_MAX) {
			continue; // !! ignore
		}

		if (!ent->name.len || (!(show & SHOW_HIDDEN) && ent->name.str[0] == '.')) {
			continue;
		}

		if (lt_lseq(ent->name, CLSTR(".")) || lt_lseq(ent->name, CLSTR(".."))) {
			continue;
		}

		max_name_len = lt_max_usz(max_name_len, ent->name.len + 2);

		lstr_t name = lt_strdup(lt_libc_heap, ent->name);
		LT_ASSERT(name.str != NULL);
		lt_darr_push(entries, name);
	}
	lt_dclose(dir, lt_libc_heap);

	sort_list(entries, lt_darr_count(entries));

	if (format == FORMAT_DEFAULT) {
		print_default(path);
	}
	else if (format == FORMAT_DETAILED_LIST) {
		print_detailed(path);
	}

	for (usz i = 0; i < lt_darr_count(entries); ++i) {
		lt_mfree(lt_libc_heap, entries[i].str);
	}
	lt_darr_destroy(entries);

	return 0;
}
