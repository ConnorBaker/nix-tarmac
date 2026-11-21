// query_hash_part STORE_URI: a C API client that keeps a store open, as a
// daemon or a program serving a store does. For each hash part read from
// stdin it prints one line: "path <name>", "none", or "error <message>".
#include <nix_api_main.h>
#include <nix_api_store.h>
#include <nix_api_util.h>

#include <stdio.h>
#include <string.h>

static void print_name(const char *start, unsigned int n, void *user_data) {
  (void)user_data;
  printf("path %.*s\n", (int)n, start);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s STORE_URI\n", argv[0]);
    return 2;
  }
  nix_c_context *ctx = nix_c_context_create();
  if (nix_libstore_init(ctx) != NIX_OK || nix_init_plugins(ctx) != NIX_OK) {
    fprintf(stderr, "init: %s\n", nix_err_msg(NULL, ctx, NULL));
    return 1;
  }
  Store *store = nix_store_open(ctx, argv[1], NULL);
  if (store == NULL) {
    fprintf(stderr, "open %s: %s\n", argv[1], nix_err_msg(NULL, ctx, NULL));
    return 1;
  }
  char line[256];
  while (fgets(line, sizeof(line), stdin) != NULL) {
    line[strcspn(line, "\n")] = '\0';
    StorePath *path = nix_store_query_path_from_hash_part(ctx, store, line);
    if (path != NULL) {
      nix_store_path_name(path, print_name, NULL);
      nix_store_path_free(path);
    } else if (nix_err_code(ctx) != NIX_OK) {
      printf("error %s\n", nix_err_msg(NULL, ctx, NULL));
    } else {
      printf("none\n");
    }
    fflush(stdout);
  }
  nix_store_free(store);
  nix_c_context_free(ctx);
  return 0;
}
