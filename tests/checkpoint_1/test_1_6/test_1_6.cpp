#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 16
#include "746FlashSim.h"

static FILE *log_file_stream;
static char log_file_path[255];

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: test_1_6 <config_file_name> <log_file_path>\n");
        exit(EXIT_FAILURE);
    }
    int ret = 1;
    strcpy(log_file_path, argv[2]);
    log_file_stream = fopen(log_file_path, "w+");
    assert(log_file_stream != NULL);

    fprintf(log_file_stream, "------------------------------------------------------------\n");

    init_flashsim();

    int r;
    FlashSimTest test(argv[1]);
    for (int i = 0; i <= BLOCK_SIZE; i++) {
        const TEST_PAGE_TYPE buffer1 = rand() % 15781;
        r = test.Write(log_file_stream, 0, buffer1);
        if (r != 1) goto failed;
        TEST_PAGE_TYPE buffer2;
        r = test.Read(log_file_stream, 0, &buffer2);
        if (r != 1) goto failed;

        if (buffer1 != buffer2) {
            fprintf(log_file_stream, "Reading LBA 0 does not return the latest data\n");
            goto failed;
        }
    }

    ret = 0;
    printf("SUCCESS ...Check %s for more details.\n", log_file_path);
    goto done;
failed:
    printf("FAILED ...Check %s for more details.\n", log_file_path);
done:
    fflush(log_file_stream);
    fclose(log_file_stream);

    deinit_flashsim();

    return ret;
}
