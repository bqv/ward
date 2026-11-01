/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/modstate.h"

int main(void) {
  char root[] = "/tmp/ward-modstate-XXXXXX";
  assert(mkdtemp(root) != NULL);
  char session[] = "0123456789abcdef";
  char dir[256];
  snprintf(dir, sizeof(dir), "%s/%s", root, session);
  assert(mkdir(dir, 0700) == 0);

  char *two = wd_module_state_path(root, session, "driver", "2");
  assert(two != NULL);
  assert(strstr(two, "driver@2.bin") != NULL);

  static const uint8_t blob[] = {0x02, 's', 't', 'a', 't', 'e'};
  assert(wd_module_state_write(two, blob, sizeof(blob)) == 0);
  uint8_t *back = NULL;
  size_t back_len = 0;
  assert(wd_module_state_read(two, &back, &back_len) == 0);
  assert(back_len == sizeof(blob));
  assert(memcmp(back, blob, sizeof(blob)) == 0);
  free(back);

  char version[32];
  assert(wd_module_state_find(root, session, "driver", version, sizeof(version)) == 0);
  assert(strcmp(version, "2") == 0);
  assert(wd_module_state_find(root, session, "provider", version, sizeof(version)) == 1);

  char *one = wd_module_state_path(root, session, "driver", "1");
  assert(one != NULL);
  assert(wd_module_state_forget(root, session, "driver", "1") == 1);
  assert(wd_module_state_find(root, session, "driver", version, sizeof(version)) == 1);

  assert(wd_module_state_path(root, session, "../driver", "1") == NULL);
  assert(wd_module_state_path(root, session, "driver", "1/2") == NULL);
  assert(wd_module_state_write(two, blob, sizeof(blob)) == 0);
  assert(wd_module_state_forget(root, session, "driver", NULL) == 1);
  assert(wd_module_state_read(two, &back, &back_len) == 1);
  assert(back == NULL);

  free(two);
  free(one);
  printf("modstate tests passed\n");
  return 0;
}
