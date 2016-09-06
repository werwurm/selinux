#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "regex.h"
#include "label_file.h"

#ifdef USE_PCRE2
struct regex_data {
	pcre2_code *regex; /* compiled regular expression */
	/*
	 * match data block required for the compiled
	 * pattern in pcre2
	 */
	pcre2_match_data *match_data;
};

int regex_prepare_data(struct regex_data **regex, char const *pattern_string,
		       struct regex_error_data *errordata)
{
	memset(errordata, 0, sizeof(struct regex_error_data));

	*regex = regex_data_create();
	if (!(*regex))
		return -1;

	(*regex)->regex = pcre2_compile(
	    (PCRE2_SPTR)pattern_string, PCRE2_ZERO_TERMINATED, PCRE2_DOTALL,
	    &errordata->error_code, &errordata->error_offset, NULL);
	if (!(*regex)->regex) {
		goto err;
	}

	(*regex)->match_data =
	    pcre2_match_data_create_from_pattern((*regex)->regex, NULL);
	if (!(*regex)->match_data) {
		goto err;
	}
	return 0;

err:
	regex_data_free(*regex);
	*regex = NULL;
	return -1;
}

char const *regex_version(void)
{
	static char version_buf[256];
	size_t len = pcre2_config(PCRE2_CONFIG_VERSION, NULL);
	if (len <= 0 || len > sizeof(version_buf))
		return NULL;

	pcre2_config(PCRE2_CONFIG_VERSION, version_buf);
	return version_buf;
}

int regex_load_mmap(struct mmap_area *mmap_area, struct regex_data **regex)
{
	int rc;
	uint32_t entry_len;

	rc = next_entry(&entry_len, mmap_area, sizeof(uint32_t));
	if (rc < 0)
		return -1;

	if (entry_len) {
		/*
		 * this should yield exactly one because we store one pattern at
		 * a time
		 */
		rc = pcre2_serialize_get_number_of_codes(mmap_area->next_addr);
		if (rc != 1)
			return -1;

		*regex = regex_data_create();
		if (!*regex)
			return -1;

		rc = pcre2_serialize_decode(&(*regex)->regex, 1,
					    (PCRE2_SPTR)mmap_area->next_addr,
					    NULL);
		if (rc != 1)
			goto err;

		(*regex)->match_data =
		    pcre2_match_data_create_from_pattern((*regex)->regex, NULL);
		if (!(*regex)->match_data)
			goto err;
	}

	/* and skip the decoded bit */
	rc = next_entry(NULL, mmap_area, entry_len);
	if (rc < 0)
		goto err;

	return 0;
err:
	regex_data_free(*regex);
	*regex = NULL;
	return -1;
}

int regex_writef(struct regex_data *regex, FILE *fp, int do_write_precompregex)
{
	int rc = 0;
	size_t len;
	PCRE2_SIZE serialized_size;
	uint32_t to_write = 0;
	PCRE2_UCHAR *bytes = NULL;

	if (do_write_precompregex) {
		/* encode the patter for serialization */
		rc = pcre2_serialize_encode((const pcre2_code **)&regex->regex,
					    1, &bytes, &serialized_size, NULL);
		if (rc != 1) {
			rc = -1;
			goto out;
		}
		to_write = serialized_size;
	}

	/* write serialized pattern's size */
	len = fwrite(&to_write, sizeof(uint32_t), 1, fp);
	if (len != 1) {
		rc = -1;
		goto out;
	}

	if (do_write_precompregex) {
		/* write serialized pattern */
		len = fwrite(bytes, 1, to_write, fp);
		if (len != to_write)
			rc = -1;
	}

out:
	if (bytes)
		pcre2_serialize_free(bytes);

	return rc;
}

void regex_data_free(struct regex_data *regex)
{
	if (regex) {
		if (regex->regex)
			pcre2_code_free(regex->regex);
		if (regex->match_data)
			pcre2_match_data_free(regex->match_data);
		free(regex);
	}
}

int regex_match(struct regex_data *regex, char const *subject, int partial)
{
	int rc;
	rc = pcre2_match(
	    regex->regex, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED, 0,
	    partial ? PCRE2_PARTIAL_SOFT : 0, regex->match_data, NULL);
	if (rc > 0)
		return REGEX_MATCH;
	switch (rc) {
	case PCRE2_ERROR_PARTIAL:
		return REGEX_MATCH_PARTIAL;
	case PCRE2_ERROR_NOMATCH:
		return REGEX_NO_MATCH;
	default:
		return REGEX_ERROR;
	}
}

/*
 * TODO Replace this compare function with something that actually compares the
 * regular expressions.
 * This compare function basically just compares the binary representations of
 * the automatons, and because this representation contains pointers and
 * metadata, it can only return a match if regex1 == regex2.
 * Preferably, this function would be replaced with an algorithm that computes
 * the equivalence of the automatons systematically.
 */
int regex_cmp(struct regex_data *regex1, struct regex_data *regex2)
{
	int rc;
	size_t len1, len2;
	rc = pcre2_pattern_info(regex1->regex, PCRE2_INFO_SIZE, &len1);
	assert(rc == 0);
	rc = pcre2_pattern_info(regex2->regex, PCRE2_INFO_SIZE, &len2);
	assert(rc == 0);
	if (len1 != len2 || memcmp(regex1->regex, regex2->regex, len1))
		return SELABEL_INCOMPARABLE;

	return SELABEL_EQUAL;
}

#else // !USE_PCRE2

/* Prior to version 8.20, libpcre did not have pcre_free_study() */
#if (PCRE_MAJOR < 8 || (PCRE_MAJOR == 8 && PCRE_MINOR < 20))
#define pcre_free_study pcre_free
#endif

struct regex_data {
	int owned;   /*
		      * non zero if regex and pcre_extra is owned by this
		      * structure and thus must be freed on destruction.
		      */
	pcre *regex; /* compiled regular expression */
	union {
		pcre_extra *sd; /* pointer to extra compiled stuff */
		pcre_extra lsd; /* used to hold the mmap'd version */
	};
};

int regex_prepare_data(struct regex_data **regex, char const *pattern_string,
		       struct regex_error_data *errordata)
{
	memset(errordata, 0, sizeof(struct regex_error_data));

	*regex = regex_data_create();
	if (!(*regex))
		return -1;

	(*regex)->regex =
	    pcre_compile(pattern_string, PCRE_DOTALL, &errordata->error_buffer,
			 &errordata->error_offset, NULL);
	if (!(*regex)->regex)
		goto err;

	(*regex)->owned = 1;

	(*regex)->sd = pcre_study((*regex)->regex, 0, &errordata->error_buffer);
	if (!(*regex)->sd && errordata->error_buffer)
		goto err;

	return 0;

err:
	regex_data_free(*regex);
	*regex = NULL;
	return -1;
}

char const *regex_version(void)
{
	return pcre_version();
}

int regex_load_mmap(struct mmap_area *mmap_area, struct regex_data **regex)
{
	int rc;
	uint32_t entry_len;
	size_t info_len;

	rc = next_entry(&entry_len, mmap_area, sizeof(uint32_t));
	if (rc < 0 || !entry_len)
		return -1;

	*regex = regex_data_create();
	if (!(*regex))
		return -1;

	(*regex)->owned = 0;
	(*regex)->regex = (pcre *)mmap_area->next_addr;
	rc = next_entry(NULL, mmap_area, entry_len);
	if (rc < 0)
		goto err;

	/*
	 * Check that regex lengths match. pcre_fullinfo()
	 * also validates its magic number.
	 */
	rc = pcre_fullinfo((*regex)->regex, NULL, PCRE_INFO_SIZE, &info_len);
	if (rc < 0 || info_len != entry_len)
		goto err;

	rc = next_entry(&entry_len, mmap_area, sizeof(uint32_t));
	if (rc < 0 || !entry_len)
		goto err;

	if (entry_len) {
		(*regex)->lsd.study_data = (void *)mmap_area->next_addr;
		(*regex)->lsd.flags |= PCRE_EXTRA_STUDY_DATA;
		rc = next_entry(NULL, mmap_area, entry_len);
		if (rc < 0)
			goto err;

		/* Check that study data lengths match. */
		rc = pcre_fullinfo((*regex)->regex, &(*regex)->lsd,
				   PCRE_INFO_STUDYSIZE, &info_len);
		if (rc < 0 || info_len != entry_len)
			goto err;
	}
	return 0;

err:
	regex_data_free(*regex);
	*regex = NULL;
	return -1;
}

static inline pcre_extra *get_pcre_extra(struct regex_data *regex)
{
	if (!regex) return NULL;
	if (regex->owned) {
		return regex->sd;
	} else if (regex->lsd.study_data) {
		return &regex->lsd;
	} else {
		return NULL;
	}
}

int regex_writef(struct regex_data *regex, FILE *fp)
{
	int rc;
	size_t len;
	uint32_t to_write;
	size_t size;
	pcre_extra *sd = get_pcre_extra(regex);

	/* determine the size of the pcre data in bytes */
	rc = pcre_fullinfo(regex->regex, NULL, PCRE_INFO_SIZE, &size);
	if (rc < 0)
		return -1;

	/* write the number of bytes in the pcre data */
	to_write = size;
	len = fwrite(&to_write, sizeof(uint32_t), 1, fp);
	if (len != 1)
		return -1;

	/* write the actual pcre data as a char array */
	len = fwrite(regex->regex, 1, to_write, fp);
	if (len != to_write)
		return -1;

	if (sd) {
		/* determine the size of the pcre study info */
		rc =
		    pcre_fullinfo(regex->regex, sd, PCRE_INFO_STUDYSIZE, &size);
		if (rc < 0)
			return -1;
	} else
		size = 0;

	/* write the number of bytes in the pcre study data */
	to_write = size;
	len = fwrite(&to_write, sizeof(uint32_t), 1, fp);
	if (len != 1)
		return -1;

	if (sd) {
		/* write the actual pcre study data as a char array */
		len = fwrite(sd->study_data, 1, to_write, fp);
		if (len != to_write)
			return -1;
	}

	return 0;
}

void regex_data_free(struct regex_data *regex)
{
	if (regex) {
		if (regex->owned) {
			if (regex->regex)
				pcre_free(regex->regex);
			if (regex->sd)
				pcre_free_study(regex->sd);
		}
		free(regex);
	}
}

int regex_match(struct regex_data *regex, char const *subject, int partial)
{
	int rc;

	rc = pcre_exec(regex->regex, get_pcre_extra(regex),
		       subject, strlen(subject), 0,
		       partial ? PCRE_PARTIAL_SOFT : 0, NULL, 0);
	switch (rc) {
	case 0:
		return REGEX_MATCH;
	case PCRE_ERROR_PARTIAL:
		return REGEX_MATCH_PARTIAL;
	case PCRE_ERROR_NOMATCH:
		return REGEX_NO_MATCH;
	default:
		return REGEX_ERROR;
	}
}

/*
 * TODO Replace this compare function with something that actually compares the
 * regular expressions.
 * This compare function basically just compares the binary representations of
 * the automatons, and because this representation contains pointers and
 * metadata, it can only return a match if regex1 == regex2.
 * Preferably, this function would be replaced with an algorithm that computes
 * the equivalence of the automatons systematically.
 */
int regex_cmp(struct regex_data *regex1, struct regex_data *regex2)
{
	int rc;
	size_t len1, len2;
	rc = pcre_fullinfo(regex1->regex, NULL, PCRE_INFO_SIZE, &len1);
	assert(rc == 0);
	rc = pcre_fullinfo(regex2->regex, NULL, PCRE_INFO_SIZE, &len2);
	assert(rc == 0);
	if (len1 != len2 || memcmp(regex1->regex, regex2->regex, len1))
		return SELABEL_INCOMPARABLE;

	return SELABEL_EQUAL;
}

#endif

struct regex_data *regex_data_create(void)
{
	return (struct regex_data *)calloc(1, sizeof(struct regex_data));
}

void regex_format_error(struct regex_error_data const *error_data, char *buffer,
			size_t buf_size)
{
	unsigned the_end_length = buf_size > 4 ? 4 : buf_size;
	char *ptr = &buffer[buf_size - the_end_length];
	int rc = 0;
	size_t pos = 0;
	if (!buffer || !buf_size)
		return;
	rc = snprintf(buffer, buf_size, "REGEX back-end error: ");
	if (rc < 0)
		/*
		 * If snprintf fails it constitutes a logical error that needs
		 * fixing.
		 */
		abort();

	pos += rc;
	if (pos >= buf_size)
		goto truncated;

	if (error_data->error_offset > 0) {
#ifdef USE_PCRE2
		rc = snprintf(buffer + pos, buf_size - pos, "At offset %zu: ",
			      error_data->error_offset);
#else
		rc = snprintf(buffer + pos, buf_size - pos, "At offset %d: ",
			      error_data->error_offset);
#endif
		if (rc < 0)
			abort();
	}
	pos += rc;
	if (pos >= buf_size)
		goto truncated;

#ifdef USE_PCRE2
	rc = pcre2_get_error_message(error_data->error_code,
				     (PCRE2_UCHAR *)(buffer + pos),
				     buf_size - pos);
	if (rc == PCRE2_ERROR_NOMEMORY)
		goto truncated;
#else
	rc = snprintf(buffer + pos, buf_size - pos, "%s",
		      error_data->error_buffer);
	if (rc < 0)
		abort();

	if ((size_t)rc < strlen(error_data->error_buffer))
		goto truncated;
#endif

	return;

truncated:
	/* replace end of string with "..." to indicate that it was truncated */
	switch (the_end_length) {
	/* no break statements, fall-through is intended */
	case 4:
		*ptr++ = '.';
	case 3:
		*ptr++ = '.';
	case 2:
		*ptr++ = '.';
	case 1:
		*ptr++ = '\0';
	default:
		break;
	}
	return;
}
