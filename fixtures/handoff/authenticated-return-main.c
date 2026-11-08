#include <string.h>

int auth_key_b_positive(void);
int auth_key_b_retab_positive(void);
int auth_key_b_bad_lr(void);
int auth_key_b_bad_sp(void);
int auth_key_b_bad_sp_nested(void);
int auth_key_b_bad_lr_retab(void);
int auth_key_b_bad_sp_retab(void);

int main(int argc, char **argv) {
  if (argc == 1) return auth_key_b_positive();
  if (argc == 2 && strcmp(argv[1], "retab") == 0) return auth_key_b_retab_positive();
  if (argc == 2 && strcmp(argv[1], "bad-lr") == 0) return auth_key_b_bad_lr();
  if (argc == 2 && strcmp(argv[1], "bad-sp") == 0) return auth_key_b_bad_sp();
  if (argc == 2 && strcmp(argv[1], "bad-sp-nested") == 0) return auth_key_b_bad_sp_nested();
  if (argc == 2 && strcmp(argv[1], "bad-lr-retab") == 0) return auth_key_b_bad_lr_retab();
  if (argc == 2 && strcmp(argv[1], "bad-sp-retab") == 0) return auth_key_b_bad_sp_retab();
  return 64;
}
