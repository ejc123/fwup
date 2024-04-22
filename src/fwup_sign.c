/*
 * Copyright 2014-2017 Frank Hunleth
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fwup_sign.h"
#include "fwfile.h"
#include "util.h"
#include "cfgfile.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef FWUP_MINIMAL

/**
 * @brief Sign a firmware update file
 * @param input_filename the firmware update filename
 * @param output_filename where to store the signed firmware update
 * @param signing_key the signing key
 * @return 0 if successful
 */
int fwup_sign(const char *input_filename, const char *output_filename, const unsigned char *signing_key)
{
    int rc = 0;
    char *configtxt = NULL;
    char buffer[4096];
    char *temp_filename = NULL;

    struct archive *in = archive_read_new();
    archive_read_support_format_zip(in);
    struct archive *out = archive_write_new();
    if (archive_write_set_format_zip(out) != ARCHIVE_OK ||
        archive_write_zip_set_compression_deflate(out) != ARCHIVE_OK)
        ERR_CLEANUP_MSG("error configuring libarchive: %s", archive_error_string(out));

    // Setting the compression-level is only supported on more recent versions
    // of libarchive, so don't check for errors.
    archive_write_set_format_option(out, "zip", "compression-level", "9");

    if (!input_filename)
        ERR_CLEANUP_MSG("Specify an input firmware file");
    if (!output_filename)
        ERR_CLEANUP_MSG("Specify an output firmware file");
    if (!signing_key)
        ERR_CLEANUP_MSG("Specify a signing key");

    size_t temp_filename_len = strlen(input_filename) + 5;
    temp_filename = malloc(temp_filename_len);
    if (!temp_filename)
        ERR_CLEANUP_MSG("Out of memory");
    snprintf(temp_filename, temp_filename_len, "%s.tmp", input_filename);

    // NOTE: Normally we'd call fwup_archive_read_open, but that function has
    // been optimized for the streaming case. That disables seeking to the
    // central directory at the end for file attributes. Old libarchive
    // versions don't process the local headers properly and this code breaks.
    rc = archive_read_open_filename(in, input_filename, 65536);
    if (rc != ARCHIVE_OK)
        ERR_CLEANUP_MSG("%s", archive_error_string(in));

    if (archive_write_open_filename(out, temp_filename) != ARCHIVE_OK)
        ERR_CLEANUP_MSG("Error creating archive '%s'", temp_filename);

    struct archive_entry *in_ae;
    while (archive_read_next_header(in, &in_ae) == ARCHIVE_OK) {
        if (strcmp(archive_entry_pathname(in_ae), "meta.conf.ed25519") == 0) {
            // Skip old signature
        } else if (strcmp(archive_entry_pathname(in_ae), "meta.conf") == 0) {
            if (configtxt)
                ERR_CLEANUP_MSG("Invalid firmware. More than one meta.conf found");

            off_t configtxt_len;
            if (archive_read_all_data(in, in_ae, &configtxt, 50000, &configtxt_len) < 0)
                ERR_CLEANUP_MSG("Error reading meta.conf from archive.");

            if (configtxt_len < 10 || configtxt_len >= 50000)
                ERR_CLEANUP_MSG("Unexpected meta.conf size: %d", configtxt_len);

            OK_OR_CLEANUP(fwfile_add_meta_conf_str(configtxt, configtxt_len, out, signing_key));
        } else {
            if (!configtxt)
                ERR_CLEANUP_MSG("Invalid firmware. meta.conf must be at the beginning of archive");

            // Normalize attributes in case extraneous ones got added via other tools
            struct archive_entry *out_ae = archive_entry_new();
            archive_entry_set_pathname(out_ae, archive_entry_pathname(in_ae));
            if (archive_entry_size_is_set(in_ae))
                archive_entry_set_size(out_ae, archive_entry_size(in_ae));
            archive_entry_set_filetype(out_ae, AE_IFREG);
            archive_entry_set_perm(out_ae, 0644);

            // Copy the file
            rc = archive_write_header(out, out_ae);
            if (rc != ARCHIVE_OK)
                ERR_CLEANUP_MSG("Error writing '%s' header to '%s'", archive_entry_pathname(out_ae), temp_filename);

            ssize_t size_left = archive_entry_size(in_ae);
            while (size_left > 0) {
                ssize_t to_read = sizeof(buffer);
                if (to_read > size_left)
                    to_read = size_left;

                ssize_t len = archive_read_data(in, buffer, to_read);
                if (len <= 0)
                    ERR_CLEANUP_MSG("Error reading '%s' in '%s'", archive_entry_pathname(in_ae), input_filename);

                if (archive_write_data(out, buffer, len) != len)
                    ERR_CLEANUP_MSG("Error writing '%s' to '%s'", archive_entry_pathname(out_ae), temp_filename);

                size_left -= len;
            }
            archive_entry_free(out_ae);
        }
    }

    if (!configtxt)
        ERR_CLEANUP_MSG("Invalid firmware. No meta.conf not found");

    // Close the files now that we're done reading and writing to them.
    archive_write_close(out);
    archive_write_free(out);
    archive_read_close(in);
    archive_read_free(in);
    out = NULL;
    in = NULL;

#ifdef _WIN32
    // On Windows, the output_file must not exist or the rename fails.
    if (unlink(output_filename) < 0 && errno != ENOENT)
        ERR_CLEANUP_MSG("Error overwriting '%s': %s", output_filename, strerror(errno));
#endif

    // Rename our output to the original file.
    if (rename(temp_filename, output_filename) < 0)
        ERR_CLEANUP_MSG("Error updating '%s': %s", output_filename, strerror(errno));
    free(temp_filename);
    temp_filename = NULL;

    fwup_output(FRAMING_TYPE_SUCCESS, 0, "");

cleanup:
    // Close the files if they're still open.
    if (out) {
        archive_write_close(out);
        archive_write_free(out);
    }
    if (in) {
        archive_read_close(in);
        archive_read_free(in);
    }

    // Only unlink the temporary file if something failed.
    if (temp_filename) {
        unlink(temp_filename);
        free(temp_filename);
    }
    if (configtxt)
        free(configtxt);

    return rc;

}
#endif // FWUP_MINIMAL
