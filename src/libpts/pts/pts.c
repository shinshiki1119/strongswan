/*
 * Copyright (C) 2011 Sansar Choinyambuu
 * HSR Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "pts.h"

#include <debug.h>
#include <crypto/hashers/hasher.h>
#include <bio/bio_writer.h>
#include <bio/bio_reader.h>

#include <trousers/tss.h>
#include <trousers/trousers.h>

#include <sys/stat.h>
#include <sys/utsname.h>
#include <errno.h>

#define PTS_BUF_SIZE	4096

/**
 * Maximum number of PCR's of TPM, TPM Spec 1.2
 */
#define PCR_MAX_NUM				24

/**
 * Number of bytes that can be saved in a PCR of TPM, TPM Spec 1.2
 */
#define PCR_LEN					20

typedef struct private_pts_t private_pts_t;

/**
 * Private data of a pts_t object.
 *
 */
struct private_pts_t {

	/**
	 * Public pts_t interface.
	 */
	pts_t public;

	/**
	 * PTS Protocol Capabilities
	 */
	pts_proto_caps_flag_t proto_caps;

	/**
	 * PTS Measurement Algorithm
	 */
	pts_meas_algorithms_t algorithm;

	/**
	 * DH Hash Algorithm
	 */
	pts_meas_algorithms_t dh_hash_algorithm;

	/**
	 * PTS Diffie-Hellman Secret
	 */
	diffie_hellman_t *dh;

	/**
	 * PTS Diffie-Hellman Initiator Nonce
	 */
	chunk_t initiator_nonce;

	/**
	 * PTS Diffie-Hellman Responder Nonce
	 */
	chunk_t responder_nonce;

	/**
	 * Secret assessment value to be used for TPM Quote as an external data
	 */
	chunk_t secret;

	/**
	 * Platform and OS Info
	 */
	char *platform_info;

	/**
	 * TRUE if IMC-PTS, FALSE if IMV-PTS
	 */
	bool is_imc;

	/**
	 * Do we have an activated TPM
	 */
	bool has_tpm;

	/**
	 * Contains a TPM_CAP_VERSION_INFO struct
	 */
	chunk_t tpm_version_info;

	/**
	 * Contains TSS Blob structure for AIK
	 */
	chunk_t aik_blob;

	/**
	 * Contains a Attestation Identity Key or Certificate
	 */
 	certificate_t *aik;

	/**
	 * Table of extended PCRs with corresponding values
	 */
	u_char* pcrs[PCR_MAX_NUM];

	/**
	 * Length of PCR registers
	 */
	size_t pcr_len;

	/**
	 * Number of extended PCR registers
	 */
	u_int32_t pcr_count;

	/**
	 * Highest extended PCR register
	 */
	u_int32_t pcr_max;

	/**
	 * Bitmap of extended PCR registers
	 */
	u_int8_t pcr_select[PCR_MAX_NUM / 8];

};

METHOD(pts_t, get_proto_caps, pts_proto_caps_flag_t,
	   private_pts_t *this)
{
	return this->proto_caps;
}

METHOD(pts_t, set_proto_caps, void,
	private_pts_t *this, pts_proto_caps_flag_t flags)
{
	this->proto_caps = flags;
	DBG2(DBG_PTS, "supported PTS protocol capabilities: %s%s%s%s%s",
		 flags & PTS_PROTO_CAPS_C ? "C" : ".",
		 flags & PTS_PROTO_CAPS_V ? "V" : ".",
		 flags & PTS_PROTO_CAPS_D ? "D" : ".",
		 flags & PTS_PROTO_CAPS_T ? "T" : ".",
		 flags & PTS_PROTO_CAPS_X ? "X" : ".");
}

METHOD(pts_t, get_meas_algorithm, pts_meas_algorithms_t,
	private_pts_t *this)
{
	return this->algorithm;
}

METHOD(pts_t, set_meas_algorithm, void,
	private_pts_t *this, pts_meas_algorithms_t algorithm)
{
	hash_algorithm_t hash_alg;

	hash_alg = pts_meas_algo_to_hash(algorithm);
	DBG2(DBG_PTS, "selected PTS measurement algorithm is %N",
				   hash_algorithm_names, hash_alg);
	if (hash_alg != HASH_UNKNOWN)
	{
		this->algorithm = algorithm;
	}
}

METHOD(pts_t, get_dh_hash_algorithm, pts_meas_algorithms_t,
	private_pts_t *this)
{
	return this->dh_hash_algorithm;
}

METHOD(pts_t, set_dh_hash_algorithm, void,
	private_pts_t *this, pts_meas_algorithms_t algorithm)
{
	hash_algorithm_t hash_alg;

	hash_alg = pts_meas_algo_to_hash(algorithm);
	DBG2(DBG_PTS, "selected DH hash algorithm is %N",
				   hash_algorithm_names, hash_alg);
	if (hash_alg != HASH_UNKNOWN)
	{
		this->dh_hash_algorithm = algorithm;
	}
}


METHOD(pts_t, create_dh_nonce, bool,
	private_pts_t *this, pts_dh_group_t group, int nonce_len)
{
	diffie_hellman_group_t dh_group;
	chunk_t *nonce;
	rng_t *rng;

	dh_group = pts_dh_group_to_ike(group);
	DBG2(DBG_PTS, "selected PTS DH group is %N",
				   diffie_hellman_group_names, dh_group);
	DESTROY_IF(this->dh);
	this->dh = lib->crypto->create_dh(lib->crypto, dh_group);

	rng = lib->crypto->create_rng(lib->crypto, RNG_STRONG);
	if (!rng)
	{
		DBG1(DBG_PTS, "no rng available");
		return FALSE;
	}
	DBG2(DBG_PTS, "nonce length is %d", nonce_len);
	nonce = this->is_imc ? &this->responder_nonce : &this->initiator_nonce;
	chunk_free(nonce);
	rng->allocate_bytes(rng, nonce_len, nonce);
	rng->destroy(rng);

	return TRUE;
}

METHOD(pts_t, get_my_public_value, void,
	private_pts_t *this, chunk_t *value, chunk_t *nonce)
{
	this->dh->get_my_public_value(this->dh, value);
	*nonce = this->is_imc ? this->responder_nonce : this->initiator_nonce;
}

METHOD(pts_t, set_peer_public_value, void,
	private_pts_t *this, chunk_t value, chunk_t nonce)
{
	this->dh->set_other_public_value(this->dh, value);

	nonce = chunk_clone(nonce);
	if (this->is_imc)
	{
		this->initiator_nonce = nonce;
	}
	else
	{
		this->responder_nonce = nonce;
	}
}

METHOD(pts_t, calculate_secret, bool,
	private_pts_t *this)
{
	hasher_t *hasher;
	hash_algorithm_t hash_alg;
	chunk_t shared_secret;

	/* Check presence of nonces */
	if (!this->initiator_nonce.len || !this->responder_nonce.len)
	{
		DBG1(DBG_PTS, "initiator and/or responder nonce is not available");
		return FALSE;
	}
	DBG3(DBG_PTS, "initiator nonce: %B", &this->initiator_nonce);
	DBG3(DBG_PTS, "responder nonce: %B", &this->responder_nonce);

	/* Calculate the DH secret */
	if (this->dh->get_shared_secret(this->dh, &shared_secret) != SUCCESS)
	{
		DBG1(DBG_PTS, "shared DH secret computation failed");
		return FALSE;
	}
	DBG3(DBG_PTS, "shared DH secret: %B", &shared_secret);

	/* Calculate the secret assessment value */
	hash_alg = pts_meas_algo_to_hash(this->dh_hash_algorithm);
	hasher = lib->crypto->create_hasher(lib->crypto, hash_alg);

	hasher->allocate_hash(hasher, chunk_from_chars('1'), NULL);
	hasher->allocate_hash(hasher, this->initiator_nonce, NULL);
	hasher->allocate_hash(hasher, this->responder_nonce, NULL);
	hasher->allocate_hash(hasher, shared_secret, &this->secret);
	hasher->destroy(hasher);

	/* The DH secret must be destroyed */
	chunk_clear(&shared_secret);

	/*
	 * Truncate the hash to 20 bytes to fit the ExternalData
	 * argument of the TPM Quote command
	 */
	this->secret.len = min(this->secret.len, 20);
	DBG3(DBG_PTS, "secret assessment value: %B", &this->secret);
	return TRUE;
}

/**
 * Print TPM 1.2 Version Info
 */
static void print_tpm_version_info(private_pts_t *this)
{
	TPM_CAP_VERSION_INFO versionInfo;
	UINT64 offset = 0;
	TSS_RESULT result;

	result = Trspi_UnloadBlob_CAP_VERSION_INFO(&offset,
						this->tpm_version_info.ptr, &versionInfo);
	if (result != TSS_SUCCESS)
	{
		DBG1(DBG_PTS, "could not parse tpm version info: tss error 0x%x",
			 result);
	}
	else
	{
		DBG2(DBG_PTS, "TPM 1.2 Version Info: Chip Version: %hhu.%hhu.%hhu.%hhu,"
					  " Spec Level: %hu, Errata Rev: %hhu, Vendor ID: %.4s",
					  versionInfo.version.major, versionInfo.version.minor,
					  versionInfo.version.revMajor, versionInfo.version.revMinor,
					  versionInfo.specLevel, versionInfo.errataRev,
					  versionInfo.tpmVendorID);
	}
}

METHOD(pts_t, get_platform_info, char*,
	private_pts_t *this)
{
	return this->platform_info;
}

METHOD(pts_t, set_platform_info, void,
	private_pts_t *this, char *info)
{
	free(this->platform_info);
	this->platform_info = strdup(info);
}

METHOD(pts_t, get_tpm_version_info, bool,
	private_pts_t *this, chunk_t *info)
{
	if (!this->has_tpm)
	{
		return FALSE;
	}
	*info = this->tpm_version_info;
	print_tpm_version_info(this);
	return TRUE;
}

METHOD(pts_t, set_tpm_version_info, void,
	private_pts_t *this, chunk_t info)
{
	this->tpm_version_info = chunk_clone(info);
	print_tpm_version_info(this);
}

METHOD(pts_t, get_pcr_len, size_t,
	private_pts_t *this)
{
	return this->pcr_len;
}

/**
 * Load an AIK Blob (TSS_TSPATTRIB_KEYBLOB_BLOB attribute)
 */
static void load_aik_blob(private_pts_t *this)
{
	char *blob_path;
	FILE *fp;
	u_int32_t aikBlobLen;

	blob_path = lib->settings->get_str(lib->settings,
						"libimcv.plugins.imc-attestation.aik_blob", NULL);

	if (blob_path)
	{
		/* Read aik key blob from a file */
		if ((fp = fopen(blob_path, "r")) == NULL)
		{
			DBG1(DBG_PTS, "unable to open AIK Blob file: %s", blob_path);
			return;
		}

		fseek(fp, 0, SEEK_END);
		aikBlobLen = ftell(fp);
		fseek(fp, 0L, SEEK_SET);

		this->aik_blob = chunk_alloc(aikBlobLen);
		if (fread(this->aik_blob.ptr, 1, aikBlobLen, fp))
		{
			DBG2(DBG_PTS, "loaded AIK Blob from '%s'", blob_path);
			DBG3(DBG_PTS, "AIK Blob: %B", &this->aik_blob);
		}
		else
		{
			DBG1(DBG_PTS, "unable to read AIK Blob file '%s'", blob_path);
		}
		fclose(fp);
		return;
	}

	DBG1(DBG_PTS, "AIK Blob is not available");
}

/**
 * Load an AIK certificate or public key
 * the certificate having precedence over the public key if both are present
 */
static void load_aik(private_pts_t *this)
{
	char *cert_path, *key_path;

	cert_path = lib->settings->get_str(lib->settings,
						"libimcv.plugins.imc-attestation.aik_cert", NULL);
	key_path = lib->settings->get_str(lib->settings,
						"libimcv.plugins.imc-attestation.aik_key", NULL);

	if (cert_path)
	{
		this->aik = lib->creds->create(lib->creds, CRED_CERTIFICATE,
									   CERT_X509, BUILD_FROM_FILE,
									   cert_path, BUILD_END);
		if (this->aik)
		{
			DBG2(DBG_PTS, "loaded AIK certificate from '%s'", cert_path);
			return;
		}
	}
	if (key_path)
	{
		this->aik = lib->creds->create(lib->creds, CRED_CERTIFICATE,
									   CERT_TRUSTED_PUBKEY, BUILD_FROM_FILE,
									   key_path, BUILD_END);
		if (this->aik)
		{
			DBG2(DBG_PTS, "loaded AIK public key from '%s'", key_path);
			return;
		}
	}

	DBG1(DBG_PTS, "neither AIK certificate nor public key is available");
}

METHOD(pts_t, get_aik, certificate_t*,
	private_pts_t *this)
{
	return this->aik;
}

METHOD(pts_t, set_aik, void,
	private_pts_t *this, certificate_t *aik)
{
	DESTROY_IF(this->aik);
	this->aik = aik->get_ref(aik);
}

METHOD(pts_t, get_aik_keyid, bool,
	private_pts_t *this, chunk_t *keyid)
{
	public_key_t *public;
	bool success;

	if (!this->aik)
	{
		DBG1(DBG_PTS, "no AIK certificate available");
		return FALSE;
	}
	public = this->aik->get_public_key(this->aik);
	if (!public)
	{
		DBG1(DBG_PTS, "no AIK public key available");
		return FALSE;
	}
	success = public->get_fingerprint(public, KEYID_PUBKEY_INFO_SHA1, keyid);
	if (!success)
	{
		DBG1(DBG_PTS, "no SHA-1 AIK public key info ID available");
	}
	public->destroy(public);

	return success;
}

METHOD(pts_t, hash_file, bool,
	private_pts_t *this, hasher_t *hasher, char *pathname, u_char *hash)
{
	u_char buffer[PTS_BUF_SIZE];
	FILE *file;
	int bytes_read;

	file = fopen(pathname, "rb");
	if (!file)
	{
		DBG1(DBG_PTS,"  file '%s' can not be opened, %s", pathname,
			 strerror(errno));
		return FALSE;
	}
	while (TRUE)
	{
		bytes_read = fread(buffer, 1, sizeof(buffer), file);
		if (bytes_read > 0)
		{
			hasher->get_hash(hasher, chunk_create(buffer, bytes_read), NULL);
		}
		else
		{
			hasher->get_hash(hasher, chunk_empty, hash);
			break;
		}
	}
	fclose(file);

	return TRUE;
}

/**
 * Get the relative filename of a fully qualified file pathname
 */
static char* get_filename(char *pathname)
{
	char *pos, *filename;

	pos = filename = pathname;
	while (pos && *(++pos) != '\0')
	{
		filename = pos;
		pos = strchr(filename, '/');
	}
	return filename;
}

METHOD(pts_t, is_path_valid, bool,
	private_pts_t *this, char *path, pts_error_code_t *error_code)
{
	struct stat st;

	*error_code = 0;

	if (!stat(path, &st))
	{
		return TRUE;
	}
	else if (errno == ENOENT || errno == ENOTDIR)
	{
		DBG1(DBG_PTS, "file/directory does not exist %s", path);
		*error_code = TCG_PTS_FILE_NOT_FOUND;
	}
	else if (errno == EFAULT)
	{
		DBG1(DBG_PTS, "bad address %s", path);
		*error_code = TCG_PTS_INVALID_PATH;
	}
	else
	{
		DBG1(DBG_PTS, "error: %s occurred while validating path: %s",
			 		   strerror(errno), path);
		return FALSE;
	}

	return TRUE;
}

METHOD(pts_t, do_measurements, pts_file_meas_t*,
	private_pts_t *this, u_int16_t request_id, char *pathname, bool is_directory)
{
	hasher_t *hasher;
	hash_algorithm_t hash_alg;
	u_char hash[HASH_SIZE_SHA384];
	chunk_t measurement;
	pts_file_meas_t *measurements;

	/* Create a hasher */
	hash_alg = pts_meas_algo_to_hash(this->algorithm);
	hasher = lib->crypto->create_hasher(lib->crypto, hash_alg);
	if (!hasher)
	{
		DBG1(DBG_PTS, "hasher %N not available", hash_algorithm_names, hash_alg);
		return NULL;
	}

	/* Create a measurement object */
	measurements = pts_file_meas_create(request_id);

	/* Link the hash to the measurement and set the measurement length */
	measurement = chunk_create(hash, hasher->get_hash_size(hasher));

	if (is_directory)
	{
		enumerator_t *enumerator;
		char *rel_name, *abs_name;
		struct stat st;

		enumerator = enumerator_create_directory(pathname);
		if (!enumerator)
		{
			DBG1(DBG_PTS,"  directory '%s' can not be opened, %s", pathname,
				 strerror(errno));
			hasher->destroy(hasher);
			measurements->destroy(measurements);
			return NULL;
		}
		while (enumerator->enumerate(enumerator, &rel_name, &abs_name, &st))
		{
			/* measure regular files only */
			if (S_ISREG(st.st_mode) && *rel_name != '.')
			{
				if (!hash_file(this, hasher, abs_name, hash))
				{
					enumerator->destroy(enumerator);
					hasher->destroy(hasher);
					measurements->destroy(measurements);
					return NULL;
				}
				DBG2(DBG_PTS, "  %#B for '%s'", &measurement, rel_name);
				measurements->add(measurements, rel_name, measurement);
			}
		}
		enumerator->destroy(enumerator);
	}
	else
	{
		char *filename;

		if (!hash_file(this, hasher, pathname, hash))
		{
			hasher->destroy(hasher);
			measurements->destroy(measurements);
			return NULL;
		}
		filename = get_filename(pathname);
		DBG2(DBG_PTS, "  %#B for '%s'", &measurement, filename);
		measurements->add(measurements, filename, measurement);
	}
	hasher->destroy(hasher);

	return measurements;
}

/**
 * Obtain statistical information describing a file
 */
static bool file_metadata(char *pathname, pts_file_metadata_t **entry)
{
	struct stat st;
	pts_file_metadata_t *this;

	this = malloc_thing(pts_file_metadata_t);

	if (stat(pathname, &st))
	{
		DBG1(DBG_PTS, "unable to obtain statistics about '%s'", pathname);
		return FALSE;
	}

	if (S_ISREG(st.st_mode))
	{
		this->type = PTS_FILE_REGULAR;
	}
	else if (S_ISDIR(st.st_mode))
	{
		this->type = PTS_FILE_DIRECTORY;
	}
	else if (S_ISCHR(st.st_mode))
	{
		this->type = PTS_FILE_CHAR_SPEC;
	}
	else if (S_ISBLK(st.st_mode))
	{
		this->type = PTS_FILE_BLOCK_SPEC;
	}
	else if (S_ISFIFO(st.st_mode))
	{
		this->type = PTS_FILE_FIFO;
	}
	else if (S_ISLNK(st.st_mode))
	{
		this->type = PTS_FILE_SYM_LINK;
	}
	else if (S_ISSOCK(st.st_mode))
	{
		this->type = PTS_FILE_SOCKET;
	}
	else
	{
		this->type = PTS_FILE_OTHER;
	}

	this->filesize = st.st_size;
	this->created =  st.st_ctime;
	this->modified = st.st_mtime;
	this->accessed = st.st_atime;
	this->owner =    st.st_uid;
	this->group =    st.st_gid;

	*entry = this;
	return TRUE;
}

METHOD(pts_t, get_metadata, pts_file_meta_t*,
	private_pts_t *this, char *pathname, bool is_directory)
{
	pts_file_meta_t *metadata;
	pts_file_metadata_t *entry;

	/* Create a metadata object */
	metadata = pts_file_meta_create();

	if (is_directory)
	{
		enumerator_t *enumerator;
		char *rel_name, *abs_name;
		struct stat st;

		enumerator = enumerator_create_directory(pathname);
		if (!enumerator)
		{
			DBG1(DBG_PTS,"  directory '%s' can not be opened, %s", pathname,
				 strerror(errno));
			metadata->destroy(metadata);
			return NULL;
		}
		while (enumerator->enumerate(enumerator, &rel_name, &abs_name, &st))
		{
			/* measure regular files only */
			if (S_ISREG(st.st_mode) && *rel_name != '.')
			{
				if (!file_metadata(abs_name, &entry))
				{
					enumerator->destroy(enumerator);
					metadata->destroy(metadata);
					return NULL;
				}
				entry->filename = strdup(rel_name);
				metadata->add(metadata, entry);
			}
		}
		enumerator->destroy(enumerator);
	}
	else
	{
		if (!file_metadata(pathname, &entry))
		{
			metadata->destroy(metadata);
			return NULL;
		}
		entry->filename = strdup(get_filename(pathname));
		metadata->add(metadata, entry);
	}

	return metadata;
}

METHOD(pts_t, read_pcr, bool,
	private_pts_t *this, u_int32_t pcr_num, chunk_t *pcr_value)
{
	TSS_HCONTEXT hContext;
	TSS_HTPM hTPM;
	TSS_RESULT result;
	chunk_t rgbPcrValue;

	bool success = FALSE;

	result = Tspi_Context_Create(&hContext);
	if (result != TSS_SUCCESS)
	{
		DBG1(DBG_PTS, "TPM context could not be created: tss error 0x%x", result);
		return FALSE;
	}

	result = Tspi_Context_Connect(hContext, NULL);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	result = Tspi_Context_GetTpmObject (hContext, &hTPM);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	result = Tspi_TPM_PcrRead(hTPM, pcr_num, (UINT32*)&rgbPcrValue.len, &rgbPcrValue.ptr);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	*pcr_value = chunk_clone(rgbPcrValue);
	DBG3(DBG_PTS, "PCR %d value:%B", pcr_num, pcr_value);
	success = TRUE;

err:
	if (!success)
	{
		DBG1(DBG_PTS, "TPM not available: tss error 0x%x", result);
	}
	Tspi_Context_FreeMemory(hContext, NULL);
	Tspi_Context_Close(hContext);

	return success;
}

METHOD(pts_t, extend_pcr, bool,
	private_pts_t *this, u_int32_t pcr_num, chunk_t input, chunk_t *output)
{
	TSS_HCONTEXT hContext;
	TSS_HTPM hTPM;
	TSS_RESULT result;
	u_int32_t pcr_length;
	chunk_t pcr_value;

	result = Tspi_Context_Create(&hContext);
	if (result != TSS_SUCCESS)
	{
		DBG1(DBG_PTS, "TPM context could not be created: tss error 0x%x",
			 result);
		return FALSE;
	}
	result = Tspi_Context_Connect(hContext, NULL);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	result = Tspi_Context_GetTpmObject (hContext, &hTPM);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}

	pcr_value = chunk_alloc(PCR_LEN);
	result = Tspi_TPM_PcrExtend(hTPM, pcr_num, PCR_LEN, input.ptr,
								NULL, &pcr_length, &pcr_value.ptr);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}

	*output = pcr_value;
	*output = chunk_clone(*output);

	DBG3(DBG_PTS, "PCR %d extended with:      %B", pcr_num, &input);
	DBG3(DBG_PTS, "PCR %d value after extend: %B", pcr_num, output);
	
	chunk_clear(&pcr_value);
	Tspi_Context_FreeMemory(hContext, NULL);
	Tspi_Context_Close(hContext);

	return TRUE;

err:
	DBG1(DBG_PTS, "TPM not available: tss error 0x%x", result);
	
	chunk_clear(&pcr_value);
	Tspi_Context_FreeMemory(hContext, NULL);
	Tspi_Context_Close(hContext);
	
	return FALSE;
}


static void clear_pcrs(private_pts_t *this)
{
	int i;

	for (i = 0; i <= this->pcr_max; i++)
	{
		free(this->pcrs[i]);
		this->pcrs[i] = NULL;
	}
	this->pcr_count = 0;
	this->pcr_max = 0;

	memset(this->pcr_select, 0x00, sizeof(this->pcr_select));
}

METHOD(pts_t, quote_tpm, bool,
	private_pts_t *this, bool use_quote2, chunk_t *pcr_comp, chunk_t *quote_sig)
{
	TSS_HCONTEXT hContext;
	TSS_HTPM hTPM;
	TSS_HKEY hAIK;
	TSS_HKEY hSRK;
	TSS_HPOLICY srkUsagePolicy;
	TSS_UUID SRK_UUID = TSS_UUID_SRK;
	BYTE secret[] = TSS_WELL_KNOWN_SECRET;
	TSS_HPCRS hPcrComposite;
	TSS_VALIDATION valData;
	TSS_RESULT result;
	chunk_t quote_info;
	BYTE* versionInfo;
	u_int32_t versionInfoSize, pcr, i = 0, f = 1;
	bool success = FALSE;

	result = Tspi_Context_Create(&hContext);
	if (result != TSS_SUCCESS)
	{
		DBG1(DBG_PTS, "TPM context could not be created: tss error 0x%x",
			 result);
		return FALSE;
	}
	result = Tspi_Context_Connect(hContext, NULL);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}
	result = Tspi_Context_GetTpmObject (hContext, &hTPM);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}

	/* Retrieve SRK from TPM and set the authentication to well known secret*/
	result = Tspi_Context_LoadKeyByUUID(hContext, TSS_PS_TYPE_SYSTEM,
									SRK_UUID, &hSRK);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}

	result = Tspi_GetPolicyObject(hSRK, TSS_POLICY_USAGE, &srkUsagePolicy);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}
	result = Tspi_Policy_SetSecret(srkUsagePolicy, TSS_SECRET_MODE_SHA1,
					20, secret);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}

	result = Tspi_Context_LoadKeyByBlob (hContext, hSRK, this->aik_blob.len,
										 this->aik_blob.ptr, &hAIK);
	if (result != TSS_SUCCESS)
	{
		goto err1;
	}

	/* Create PCR composite object */
	result = use_quote2 ?
			Tspi_Context_CreateObject(hContext, TSS_OBJECT_TYPE_PCRS,
							TSS_PCRS_STRUCT_INFO_SHORT, &hPcrComposite) :
			Tspi_Context_CreateObject(hContext, TSS_OBJECT_TYPE_PCRS,
							TSS_PCRS_STRUCT_DEFAULT, &hPcrComposite);
	if (result != TSS_SUCCESS)
	{
		goto err2;
	}

	/* Select PCRs */
	for (pcr = 0; pcr <= this->pcr_max ; pcr++)
	{
		if (f == 256)
		{
			i++;
			f = 1;
		}		
		if (this->pcr_select[i] & f)
		{
			result = use_quote2 ?
					Tspi_PcrComposite_SelectPcrIndexEx(hPcrComposite, pcr,
												TSS_PCRS_DIRECTION_RELEASE) :
					Tspi_PcrComposite_SelectPcrIndex(hPcrComposite, pcr);
			if (result != TSS_SUCCESS)
			{
				goto err3;
			}
		}
		f <<= 1;
	}

	/* Set the Validation Data */
	valData.ulExternalDataLength = this->secret.len;
	valData.rgbExternalData = (BYTE *)this->secret.ptr;


	/* TPM Quote */
	result = use_quote2 ?
			Tspi_TPM_Quote2(hTPM, hAIK, FALSE, hPcrComposite, &valData,
							&versionInfoSize, &versionInfo):
			Tspi_TPM_Quote(hTPM, hAIK, hPcrComposite, &valData);
	if (result != TSS_SUCCESS)
	{
		goto err4;
	}

	/* Set output chunks */
	*pcr_comp = chunk_alloc(HASH_SIZE_SHA1);

	if (use_quote2)
	{
		/* TPM_Composite_Hash is last 20 bytes of TPM_Quote_Info2 structure */
		memcpy(pcr_comp->ptr, valData.rgbData + valData.ulDataLength - HASH_SIZE_SHA1,
			   HASH_SIZE_SHA1);
	}
	else
	{
		/* TPM_Composite_Hash is 8-28th bytes of TPM_Quote_Info structure */
		memcpy(pcr_comp->ptr, valData.rgbData + 8, HASH_SIZE_SHA1);
	}
	DBG3(DBG_PTS, "Hash of PCR Composite: %#B", pcr_comp);

	quote_info = chunk_create(valData.rgbData, valData.ulDataLength);
	DBG3(DBG_PTS, "TPM Quote Info: %B",&quote_info);

	*quote_sig = chunk_clone(chunk_create(valData.rgbValidationData,
							  			   valData.ulValidationDataLength));
	DBG3(DBG_PTS, "TPM Quote Signature: %B",quote_sig);

	success = TRUE;

	/* Cleanup */
err4:
	Tspi_Context_FreeMemory(hContext, NULL);

err3:
	Tspi_Context_CloseObject(hContext, hPcrComposite);

err2:
	Tspi_Context_CloseObject(hContext, hAIK);

err1:
	Tspi_Context_Close(hContext);

	if (!success)
	{
		DBG1(DBG_PTS, "TPM not available: tss error 0x%x", result);
	}
	clear_pcrs(this);

	return success;
}

METHOD(pts_t, select_pcr, bool,
	private_pts_t *this, u_int32_t pcr)
{
	u_int32_t i, f;

	if (pcr >= PCR_MAX_NUM)
	{
		DBG1(DBG_PTS, "PCR %u: number is larger than maximum of %u",
					   pcr, PCR_MAX_NUM-1);
		return FALSE;
	}

	/* Determine PCR selection flag */
	i = pcr / 8;
	f = 1 << (pcr - 8*i);

	/* Has this PCR already been selected? */
	if (!(this->pcr_select[i] & f))
	{
		this->pcr_select[i] |= f;
		this->pcr_max = max(this->pcr_max, pcr);
		this->pcr_count++;
	}

	return TRUE;
}

METHOD(pts_t, add_pcr, bool,
	private_pts_t *this, u_int32_t pcr, chunk_t pcr_before, chunk_t pcr_after)
{
	if (pcr >= PCR_MAX_NUM)
	{
		DBG1(DBG_PTS, "PCR %u: number is larger than maximum of %u",
					   pcr, PCR_MAX_NUM-1);
		return FALSE;
	}

	/* Is the length of the PCR registers already set? */
	if (this->pcr_len)
	{
		if (pcr_after.len != this->pcr_len)
		{
			DBG1(DBG_PTS, "PCR %02u: length is %d bytes but should be %d bytes",
						   pcr_after.len, this->pcr_len);
			return FALSE;
		}
	}
	else
	{
		this->pcr_len = pcr_after.len;
	}

	/* Has the value of the PCR register already been assigned? */
	if (this->pcrs[pcr])
	{
		if (!memeq(this->pcrs[pcr], pcr_before.ptr, this->pcr_len))
		{
			DBG1(DBG_PTS, "PCR %02u: new pcr_before value does not equal "
						  "old pcr_after value");
		}
		/* remove the old PCR value */
		free(this->pcrs[pcr]);
	}
	else
	{
		/* add extended PCR Register */
		this->pcr_select[pcr / 8] |= 1 << (pcr % 8);
		this->pcr_max = max(this->pcr_max, pcr);
		this->pcr_count++;
	}

	/* Duplicate and store current PCR value */
	pcr_after = chunk_clone(pcr_after);
	this->pcrs[pcr] = pcr_after.ptr;

	return TRUE;
}

/**
 * TPM_QUOTE_INFO structure:
 *	4 bytes of version
 *	4 bytes 'Q' 'U' 'O' 'T'
 *	20 byte SHA1 of TCPA_PCR_COMPOSITE
 *	20 byte nonce
 *
 * TPM_QUOTE_INFO2 structure:
 * 2 bytes Tag 0x0036 TPM_Tag_Quote_info2
 * 4 bytes 'Q' 'U' 'T' '2'
 * 20 bytes nonce
 * 26 bytes PCR_INFO_SHORT
 */

METHOD(pts_t, get_quote_info, bool,
	private_pts_t *this, bool use_quote2, bool use_ver_info,
	pts_meas_algorithms_t comp_hash_algo,
	chunk_t *out_pcr_comp, chunk_t *out_quote_info)
{
	u_int8_t size_of_select;
	int pcr_comp_len, i;
	chunk_t pcr_comp, hash_pcr_comp;
	bio_writer_t *writer;
	hasher_t *hasher;

	if (this->pcr_count == 0)
	{
		DBG1(DBG_PTS, "No extended PCR entries available, "
					  "unable to construct TPM Quote Info");
		return FALSE;
	}
	if (!this->secret.ptr)
	{
		DBG1(DBG_PTS, "Secret assessment value unavailable, ",
					  "unable to construct TPM Quote Info");
		return FALSE;
	}
	if (use_quote2 && use_ver_info && !this->tpm_version_info.ptr)
	{
		DBG1(DBG_PTS, "TPM Version Information unavailable, ",
					  "unable to construct TPM Quote Info2");
		return FALSE;
	}
	
	/**
	 * A TPM v1.2 has 24 PCR Registers
	 * so the bitmask field length used by TrouSerS is at least 3 bytes
	 */
	size_of_select = max(PCR_MAX_NUM / 8, 1 + this->pcr_max / 8);
	pcr_comp_len = 2 + size_of_select + 4 + this->pcr_count * this->pcr_len;
	
	writer = bio_writer_create(pcr_comp_len);

	writer->write_uint16(writer, size_of_select);
	for (i = 0; i < size_of_select; i++)
	{
		writer->write_uint8(writer, this->pcr_select[i]);
	}

	writer->write_uint32(writer, this->pcr_count * this->pcr_len);
	for (i = 0; i < 8 * size_of_select; i++)
	{
		if (this->pcrs[i])
		{
			writer->write_data(writer, chunk_create(this->pcrs[i], this->pcr_len));
		}
	}
	pcr_comp = chunk_clone(writer->get_buf(writer));
	DBG3(DBG_PTS, "constructed PCR Composite: %B", &pcr_comp);

	writer->destroy(writer);

	/* Output the TPM_PCR_COMPOSITE expected from IMC */
	if (comp_hash_algo)
	{
		hash_algorithm_t algo;

		algo = pts_meas_algo_to_hash(comp_hash_algo);
		hasher = lib->crypto->create_hasher(lib->crypto, algo);

		/* Hash the PCR Composite Structure */
		hasher->allocate_hash(hasher, pcr_comp, out_pcr_comp);
		DBG3(DBG_PTS, "constructed PCR Composite hash: %#B", out_pcr_comp);
		hasher->destroy(hasher);
	}
	else
	{
		*out_pcr_comp = chunk_clone(pcr_comp);
	}

	/* SHA1 hash of PCR Composite to construct TPM_QUOTE_INFO */
	hasher = lib->crypto->create_hasher(lib->crypto, HASH_SHA1);
	hasher->allocate_hash(hasher, pcr_comp, &hash_pcr_comp);
	hasher->destroy(hasher);

	/* Construct TPM_QUOTE_INFO/TPM_QUOTE_INFO2 structure */
	writer = bio_writer_create(TPM_QUOTE_INFO_LEN);

	if (use_quote2)
	{
		/* TPM Structure Tag */
		writer->write_uint16(writer, TPM_TAG_QUOTE_INFO2);

		/* Magic QUT2 value */
		writer->write_data(writer, chunk_create("QUT2", 4));

		/* Secret assessment value 20 bytes (nonce) */
		writer->write_data(writer, this->secret);

		/* Length of the PCR selection field */
		writer->write_uint16(writer, size_of_select);

		/* PCR selection */
		for (i = 0; i < size_of_select ; i++)
		{
			writer->write_uint8(writer, this->pcr_select[i]);
		}
		
		/* TPM Locality Selection */
		writer->write_uint8(writer, TPM_LOC_ZERO);

		/* PCR Composite Hash */
		writer->write_data(writer, hash_pcr_comp);

		if (use_ver_info)
		{
			/* TPM version Info */
			writer->write_data(writer, this->tpm_version_info);
		}
	}
	else
	{
		/* Version number */
		writer->write_data(writer, chunk_from_chars(1, 1, 0, 0));

		/* Magic QUOT value */
		writer->write_data(writer, chunk_create("QUOT", 4));

		/* PCR Composite Hash */
		writer->write_data(writer, hash_pcr_comp);

		/* Secret assessment value 20 bytes (nonce) */
		writer->write_data(writer, this->secret);
	}

	/* TPM Quote Info */
	*out_quote_info = chunk_clone(writer->get_buf(writer));
	DBG3(DBG_PTS, "constructed TPM Quote Info: %B", out_quote_info);

	writer->destroy(writer);
	free(pcr_comp.ptr);
	free(hash_pcr_comp.ptr);
	clear_pcrs(this);

	return TRUE;
}

METHOD(pts_t, verify_quote_signature, bool,
				private_pts_t *this, chunk_t data, chunk_t signature)
{
	public_key_t *aik_pub_key;

	aik_pub_key = this->aik->get_public_key(this->aik);
	if (!aik_pub_key)
	{
		DBG1(DBG_PTS, "failed to get public key from AIK certificate");
		return FALSE;
	}

	if (!aik_pub_key->verify(aik_pub_key, SIGN_RSA_EMSA_PKCS1_SHA1,
		data, signature))
	{
		DBG1(DBG_PTS, "signature verification failed for TPM Quote Info");
		DESTROY_IF(aik_pub_key);
		return FALSE;
	}

	aik_pub_key->destroy(aik_pub_key);
	return TRUE;
}

METHOD(pts_t, destroy, void,
	private_pts_t *this)
{
	clear_pcrs(this);
	DESTROY_IF(this->aik);
	DESTROY_IF(this->dh);
	free(this->initiator_nonce.ptr);
	free(this->responder_nonce.ptr);
	free(this->secret.ptr);
	free(this->platform_info);
	free(this->aik_blob.ptr);
	free(this->tpm_version_info.ptr);
	free(this);
}

#define RELEASE_LSB		0
#define RELEASE_DEBIAN	1

/**
 * Determine Linux distribution and hardware platform
 */
static char* extract_platform_info(void)
{
	FILE *file;
	char buf[BUF_LEN], *pos = buf, *value = NULL;
	int i, len = BUF_LEN - 1;
	struct utsname uninfo;

	/* Linux/Unix distribution release info (from http://linuxmafia.com) */
	const char* releases[] = {
		"/etc/lsb-release",           "/etc/debian_version",
		"/etc/SuSE-release",          "/etc/novell-release",
		"/etc/sles-release",          "/etc/redhat-release",
		"/etc/fedora-release",        "/etc/gentoo-release",
		"/etc/slackware-version",     "/etc/annvix-release",
		"/etc/arch-release",          "/etc/arklinux-release",
		"/etc/aurox-release",         "/etc/blackcat-release",
		"/etc/cobalt-release",        "/etc/conectiva-release",
		"/etc/debian_release",        "/etc/immunix-release",
		"/etc/lfs-release",           "/etc/linuxppc-release",
		"/etc/mandrake-release",      "/etc/mandriva-release",
		"/etc/mandrakelinux-release", "/etc/mklinux-release",
		"/etc/pld-release",           "/etc/redhat_version",
		"/etc/slackware-release",     "/etc/e-smith-release",
		"/etc/release",               "/etc/sun-release",
		"/etc/tinysofa-release",      "/etc/turbolinux-release",
		"/etc/ultrapenguin-release",  "/etc/UnitedLinux-release",
		"/etc/va-release",            "/etc/yellowdog-release"
	};

	const char description[] = "DISTRIB_DESCRIPTION=\"";
	const char str_debian[] = "Debian ";

	for (i = 0; i < countof(releases); i++)
	{
		file = fopen(releases[i], "r");
		if (!file)
		{
			continue;
		}

		if (i == RELEASE_DEBIAN)
		{
			strcpy(buf, str_debian);
			pos += strlen(str_debian);
			len -= strlen(str_debian); 
		}

		fseek(file, 0, SEEK_END);
		len = min(ftell(file), len);
		rewind(file);
		pos[len] = '\0';
		if (fread(pos, 1, len, file) != len)
		{
			DBG1(DBG_PTS, "failed to read file '%s'", releases[i]);
			fclose(file);
			return NULL;
		}
		fclose(file);

		if (i == RELEASE_LSB)
		{
			pos = strstr(buf, description);
			if (!pos)
			{
				DBG1(DBG_PTS, "failed to find begin of lsb-release "
							  "DESCRIPTION field");
				return NULL;
			}
			value = pos + strlen(description);
			pos = strchr(value, '"');
			if (!pos)
			{
				DBG1(DBG_PTS, "failed to find end of lsb-release "
							  "DESCRIPTION field");
				return NULL;
			 }
		}
		else
		{
			value = buf;
			pos = strchr(pos, '\n');
			if (!pos)
			{
				DBG1(DBG_PTS, "failed to find end of release string");
				return NULL;
			 }
		}
		break;
	}

	if (!value)
	{
		DBG1(DBG_PTS, "no distribution release file found");
		return NULL;
	}

	if (uname(&uninfo) < 0)
	{
		DBG1(DBG_PTS, "could not retrieve machine architecture");
		return NULL;
	}

	*pos++ = ' ';
	len = sizeof(buf)-1 + (pos - buf);
	strncpy(pos, uninfo.machine, len);

	DBG1(DBG_PTS, "platform is '%s'", value);
	return strdup(value);
}

/**
 * Check for a TPM by querying for TPM Version Info
 */
static bool has_tpm(private_pts_t *this)
{
	TSS_HCONTEXT hContext;
	TSS_HTPM hTPM;
	TSS_RESULT result;
	u_int32_t version_info_len;

	result = Tspi_Context_Create(&hContext);
	if (result != TSS_SUCCESS)
	{
		DBG1(DBG_PTS, "TPM context could not be created: tss error 0x%x",
			 result);
		return FALSE;
	}
	result = Tspi_Context_Connect(hContext, NULL);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	result = Tspi_Context_GetTpmObject (hContext, &hTPM);
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	result = Tspi_TPM_GetCapability(hTPM, TSS_TPMCAP_VERSION_VAL,  0, NULL,
									&version_info_len,
									&this->tpm_version_info.ptr);
	this->tpm_version_info.len = version_info_len;
	if (result != TSS_SUCCESS)
	{
		goto err;
	}
	this->tpm_version_info = chunk_clone(this->tpm_version_info);

	Tspi_Context_FreeMemory(hContext, NULL);
	Tspi_Context_Close(hContext);
	return TRUE;

	err:
	DBG1(DBG_PTS, "TPM not available: tss error 0x%x", result);
	Tspi_Context_FreeMemory(hContext, NULL);
	Tspi_Context_Close(hContext);
	return FALSE;
}

/**
 * See header
 */
pts_t *pts_create(bool is_imc)
{
	private_pts_t *this;

	INIT(this,
		.public = {
			.get_proto_caps = _get_proto_caps,
			.set_proto_caps = _set_proto_caps,
			.get_meas_algorithm = _get_meas_algorithm,
			.set_meas_algorithm = _set_meas_algorithm,
			.get_dh_hash_algorithm = _get_dh_hash_algorithm,
			.set_dh_hash_algorithm = _set_dh_hash_algorithm,
			.create_dh_nonce = _create_dh_nonce,
			.get_my_public_value = _get_my_public_value,
			.set_peer_public_value = _set_peer_public_value,
			.calculate_secret = _calculate_secret,
			.get_platform_info = _get_platform_info,
			.set_platform_info = _set_platform_info,
			.get_tpm_version_info = _get_tpm_version_info,
			.set_tpm_version_info = _set_tpm_version_info,
			.get_pcr_len = _get_pcr_len,
			.get_aik = _get_aik,
			.set_aik = _set_aik,
			.get_aik_keyid = _get_aik_keyid,
			.is_path_valid = _is_path_valid,
			.hash_file = _hash_file,
			.do_measurements = _do_measurements,
			.get_metadata = _get_metadata,
			.read_pcr = _read_pcr,
			.extend_pcr = _extend_pcr,
			.quote_tpm = _quote_tpm,
			.select_pcr = _select_pcr,
			.add_pcr = _add_pcr,
			.get_quote_info = _get_quote_info,
			.verify_quote_signature  = _verify_quote_signature,
			.destroy = _destroy,
		},
		.is_imc = is_imc,
		.proto_caps = PTS_PROTO_CAPS_V,
		.algorithm = PTS_MEAS_ALGO_SHA256,
		.dh_hash_algorithm = PTS_MEAS_ALGO_SHA256,
	);

	if (is_imc)
	{
		this->platform_info = extract_platform_info();

		if (has_tpm(this))
		{
			this->has_tpm = TRUE;
			this->pcr_len = PCR_LEN;
			this->proto_caps |= PTS_PROTO_CAPS_T | PTS_PROTO_CAPS_D;
			load_aik(this);
			load_aik_blob(this);
		}
	}
	else
	{
		this->proto_caps |= PTS_PROTO_CAPS_T | PTS_PROTO_CAPS_D;
	}

	return &this->public;
}
