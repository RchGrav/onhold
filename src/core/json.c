#include "hold/config.h"
#include "hold/types.h"
#include "hold/core.h"

static int skip_json_string(const char **pp);
static int match_json_string(const char *p, const char *lit, const char **endp, bool *matched);
static int skip_json_value_impl(const char **pp, int depth);
static int json_get_string_array_alloc_impl(const char *j, const char *key, bool allow_empty, char ***argv_out, int *argc_out);

void hold_json_escape(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') {
            fprintf(f, "\\%c", *s);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else if (*s == '\r') {
            fputs("\\r", f);
        } else if (*s == '\t') {
            fputs("\\t", f);
        } else if (*s == '\b') {
            fputs("\\b", f);
        } else if (*s == '\f') {
            fputs("\\f", f);
        } else if ((unsigned char)*s < 32) {
            fprintf(f, "\\u%04x", (unsigned char)*s);
        } else {
            fputc(*s, f);
        }
    }
}

int hold_write_json_argv(FILE *f, int argc, char **argv) {
    fputs("[", f);
    for (int i = 0; i < argc; i++) {
        if (i > 0) {
            fputs(", ", f);
        }
        fputc('"', f);
        hold_json_escape(f, argv[i]);
        fputc('"', f);
    }
    fputs("]", f);
    return 0;
}

const char *hold_skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int skip_json_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return -1;
    p++;
    while (*p) {
        if (*p == '"') {
            *pp = p + 1;
            return 0;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            if (*p == 'u') {
                for (int i = 0; i < 4; i++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                }
            }
        }
        p++;
    }
    return -1;
}

/* BMP-only; surrogate pairs are rejected. */
int hold_parse_json_string(const char *p, char *out, size_t n, const char **endp) {
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p) {
        if (*p == '"') {
            if (i >= n) return -1;
            out[i] = '\0';
            if (endp) *endp = p + 1;
            return 0;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            char c = *p;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\\': case '"': case '/': break;
            case 'u': {
                unsigned v = 0;
                for (int j = 0; j < 4; j++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                    v = (v << 4) + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                if (v == 0) return -1;
                if (v >= 0xD800 && v <= 0xDFFF) return -1;
                if (v <= 0x7F) {
                    c = (char)v;
                    if (i + 1 >= n) return -1;
                    out[i++] = c;
                } else if (v <= 0x7FF) {
                    if (i + 2 >= n) return -1;
                    out[i++] = (char)(0xC0 | (v >> 6));
                    out[i++] = (char)(0x80 | (v & 0x3F));
                } else {
                    if (i + 3 >= n) return -1;
                    out[i++] = (char)(0xE0 | (v >> 12));
                    out[i++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    out[i++] = (char)(0x80 | (v & 0x3F));
                }
                p++;
                continue;
            }
            default: return -1;
            }
            if (i + 1 >= n) return -1;
            out[i++] = c;
            p++;
            continue;
        }
        if (i + 1 >= n) return -1;
        out[i++] = *p++;
    }
    return -1;
}

static int match_json_string(const char *p, const char *lit, const char **endp, bool *matched) {
    if (*p != '"') return -1;
    p++;
    size_t li = 0;
    bool ok = true;
    while (*p) {
        if (*p == '"') {
            if (lit[li] != '\0') {
                ok = false;
            }
            if (endp) *endp = p + 1;
            if (matched) *matched = ok;
            return 0;
        }
        unsigned cp = 0;
        if (*p == '\\') {
            p++;
            if (!*p) return -1;
            switch (*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = '\t'; p++; break;
            case 'r': cp = '\r'; p++; break;
            case 'b': cp = '\b'; p++; break;
            case 'f': cp = '\f'; p++; break;
            case '\\': cp = '\\'; p++; break;
            case '"': cp = '"'; p++; break;
            case '/': cp = '/'; p++; break;
            case 'u': {
                unsigned v = 0;
                for (int j = 0; j < 4; j++) {
                    p++;
                    if (!isxdigit((unsigned char)*p)) return -1;
                    v = (v << 4) + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                }
                if (v == 0 || (v >= 0xD800 && v <= 0xDFFF)) return -1;
                cp = v;
                p++;
                break;
            }
            default:
                return -1;
            }
        } else {
            cp = (unsigned char)*p;
            p++;
        }

        if (cp <= 0x7F) {
            if (lit[li] == '\0' || (unsigned char)lit[li] != cp) {
                ok = false;
            }
            if (lit[li] != '\0') {
                li++;
            }
        } else {
            ok = false;
        }
    }
    return -1;
}

static int skip_json_value_impl(const char **pp, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        errno = EINVAL;
        return -1;
    }

    const char *p = hold_skip_ws(*pp);
    if (*p == '"') {
        if (skip_json_string(&p) != 0) return -1;
        *pp = p;
        return 0;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        p++;
        while (*p) {
            p = hold_skip_ws(p);
            if (*p == close) { *pp = p + 1; return 0; }
            if (open == '{') {
                if (skip_json_string(&p) != 0) return -1;
                p = hold_skip_ws(p);
                if (*p != ':') return -1;
                p++;
            }
            if (skip_json_value_impl(&p, depth + 1) != 0) return -1;
            p = hold_skip_ws(p);
            if (*p == ',') p++;
        }
        return -1;
    }
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != '}' && *p != ']') p++;
    *pp = p;
    return 0;
}

int hold_skip_json_value(const char **pp) {
    return skip_json_value_impl(pp, 0);
}

int hold_json_find_key(const char *j, const char *k, const char **v) {
    const char *p = hold_skip_ws(j);
    if (*p != '{') return -1;
    p++;
    while (*p) {
        p = hold_skip_ws(p);
        if (*p == '}') return -1;
        bool key_match = false;
        if (match_json_string(p, k, &p, &key_match) != 0) return -1;
        p = hold_skip_ws(p);
        if (*p != ':') return -1;
        p = hold_skip_ws(p + 1);
        if (key_match) {
            *v = p;
            return 0;
        }
        if (hold_skip_json_value(&p) != 0) return -1;
        p = hold_skip_ws(p);
        if (*p == ',') p++;
    }
    return -1;
}

int hold_json_get_i64(const char *j, const char *k, int64_t *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) {
        return -1;
    }
    if (*v == '+') return -1;
    char *end = NULL;
    errno = 0;
    long long x = strtoll(v, &end, 10);
    if (end == v || errno != 0) return -1;
    end = (char *)hold_skip_ws(end);
    if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
    *out = x;
    return 0;
}

int hold_json_get_bool(const char *j, const char *k, bool *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) {
        return -1;
    }
    v = hold_skip_ws(v);
    if (strncmp(v, "true", 4) == 0) {
        const char *end = hold_skip_ws(v + 4);
        if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
        *out = true;
        return 0;
    }
    if (strncmp(v, "false", 5) == 0) {
        const char *end = hold_skip_ws(v + 5);
        if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
        *out = false;
        return 0;
    }
    return -1;
}

int hold_json_get_u64(const char *j, const char *k, uint64_t *out) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) {
        return -1;
    }
    if (*v == '+' || *v == '-') return -1;
    char *end = NULL;
    errno = 0;
    unsigned long long x = strtoull(v, &end, 10);
    if (end == v || errno != 0) return -1;
    end = (char *)hold_skip_ws(end);
    if (*end && *end != ',' && *end != '}' && *end != ']') return -1;
    *out = x;
    return 0;
}

int hold_json_get_str(const char *j, const char *k, char *out, size_t n) {
    const char *v;
    if (hold_json_find_key(j, k, &v) != 0) return -1;
    return hold_parse_json_string(hold_skip_ws(v), out, n, NULL);
}

int hold_json_get_argv_display(const char *j, char *out, size_t n) {
    const char *v;
    if (hold_json_find_key(j, "argv", &v) != 0 || *v != '[') {
        return -1;
    }
    v = hold_skip_ws(v + 1);
    size_t off = 0;
    bool first = true;
    while (*v && *v != ']') {
        char arg[HOLD_PATH_MAX];
        if (hold_parse_json_string(v, arg, sizeof(arg), &v) != 0) {
            return -1;
        }
        if (!first) {
            if (off + 1 >= n) return -1;
            out[off++] = ' ';
            out[off] = '\0';
        }
        if (hold_append_cmd_human(out, n, &off, arg) != 0) {
            return -1;
        }
        first = false;
        v = hold_skip_ws(v);
        if (*v == ',') {
            v = hold_skip_ws(v + 1);
        } else if (*v != ']') {
            return -1;
        }
    }
    if (*v != ']') return -1;
    return 0;
}

void hold_free_argv_alloc(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static int json_get_string_array_alloc_impl(const char *j, const char *key, bool allow_empty, char ***argv_out, int *argc_out) {
    *argv_out = NULL;
    *argc_out = 0;
    const char *v;
    if (hold_json_find_key(j, key, &v) != 0 || *v != '[') {
        return -1;
    }
    v = hold_skip_ws(v + 1);
    int cap = 4;
    int argc = 0;
    char **argv = calloc((size_t)cap + 1, sizeof(char *));
    if (!argv) {
        return -1;
    }
    while (*v && *v != ']') {
        char arg[HOLD_PATH_MAX];
        if (hold_parse_json_string(v, arg, sizeof(arg), &v) != 0) {
            hold_free_argv_alloc(argv, argc);
            return -1;
        }
        if (argc == cap) {
            cap *= 2;
            char **next = realloc(argv, ((size_t)cap + 1) * sizeof(char *));
            if (!next) {
                hold_free_argv_alloc(argv, argc);
                return -1;
            }
            argv = next;
        }
        argv[argc] = strdup(arg);
        if (!argv[argc]) {
            hold_free_argv_alloc(argv, argc);
            return -1;
        }
        argc++;
        argv[argc] = NULL;
        v = hold_skip_ws(v);
        if (*v == ',') {
            v = hold_skip_ws(v + 1);
        } else if (*v != ']') {
            hold_free_argv_alloc(argv, argc);
            return -1;
        }
    }
    if (*v != ']' || (!allow_empty && argc == 0)) {
        hold_free_argv_alloc(argv, argc);
        return -1;
    }
    if (argc == 0) {
        free(argv);
        argv = NULL;
    }
    *argv_out = argv;
    *argc_out = argc;
    return 0;
}

static int json_get_string_array_alloc(const char *j, const char *key, char ***argv_out, int *argc_out) {
    return json_get_string_array_alloc_impl(j, key, false, argv_out, argc_out);
}

int hold_json_get_argv_alloc(const char *j, char ***argv_out, int *argc_out) {
    return json_get_string_array_alloc(j, "argv", argv_out, argc_out);
}

int hold_json_get_args_alloc(const char *j, char ***argv_out, int *argc_out) {
    return json_get_string_array_alloc(j, "args", argv_out, argc_out);
}

int hold_json_get_env_alloc(const char *j, char ***env_out, int *envc_out) {
    return json_get_string_array_alloc(j, "env", env_out, envc_out);
}

int hold_json_get_ports_alloc(const char *j, char ***ports_out, int *portc_out) {
    return json_get_string_array_alloc(j, "ports", ports_out, portc_out);
}

int hold_json_get_volumes_alloc(const char *j, char ***volumes_out, int *volumec_out) {
    return json_get_string_array_alloc(j, "volumes", volumes_out, volumec_out);
}

int hold_json_get_string_array_key_alloc(const char *j, const char *key, char ***items_out, int *count_out) {
    return json_get_string_array_alloc(j, key, items_out, count_out);
}

int hold_json_get_string_array_key_allow_empty_alloc(const char *j, const char *key, char ***items_out, int *count_out) {
    return json_get_string_array_alloc_impl(j, key, true, items_out, count_out);
}

int hold_json_get_path_args_argv_alloc(const char *j, char ***argv_out, int *argc_out) {
    *argv_out = NULL;
    *argc_out = 0;
    char path[HOLD_PATH_MAX];
    if (hold_json_get_str(j, "Path", path, sizeof(path)) != 0 || path[0] == '\0') {
        return -1;
    }
    char **args = NULL;
    int arg_count = 0;
    if (hold_json_get_string_array_key_allow_empty_alloc(j, "Args", &args, &arg_count) != 0) {
        args = NULL;
        arg_count = 0;
    }
    char **argv = calloc((size_t)arg_count + 2, sizeof(char *));
    if (!argv) {
        hold_free_argv_alloc(args, arg_count);
        return -1;
    }
    argv[0] = strdup(path);
    if (!argv[0]) {
        free(argv);
        hold_free_argv_alloc(args, arg_count);
        return -1;
    }
    for (int i = 0; i < arg_count; i++) {
        argv[i + 1] = strdup(args[i]);
        if (!argv[i + 1]) {
            hold_free_argv_alloc(argv, i + 1);
            hold_free_argv_alloc(args, arg_count);
            return -1;
        }
    }
    argv[arg_count + 1] = NULL;
    hold_free_argv_alloc(args, arg_count);
    *argv_out = argv;
    *argc_out = arg_count + 1;
    return 0;
}

int hold_copy_argv(char ***out, int argc, char **argv) {
    *out = NULL;
    if (argc <= 0 || !argv) {
        errno = EINVAL;
        return -1;
    }
    char **copy = calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        copy[i] = strdup(argv[i]);
        if (!copy[i]) {
            hold_free_argv_alloc(copy, i);
            return -1;
        }
    }
    copy[argc] = NULL;
    *out = copy;
    return 0;
}
