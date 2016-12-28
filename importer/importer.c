#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipfs/importer/importer.h"
#include "ipfs/merkledag/merkledag.h"
#include "ipfs/repo/fsrepo/fs_repo.h"
#include "ipfs/unixfs/unixfs.h"

#define MAX_DATA_SIZE 262144 // 1024 * 256;

/***
 * Imports OS files into the datastore
 */

/***
 * adds a blocksize to the UnixFS structure stored in the data
 * element of a Node
 * @param node the node to work with
 * @param blocksize the blocksize to add
 * @returns true(1) on success
 */
int ipfs_importer_add_filesize_to_data_section(struct Node* node, size_t bytes_read) {
	// now add to the data section
	struct UnixFS* data_section = NULL;
	if (node->data == NULL) {
		// nothing in data section yet, create new UnixFS
		ipfs_unixfs_new(&data_section);
		data_section->data_type = UNIXFS_FILE;
	} else {
		ipfs_unixfs_protobuf_decode(node->data, node->data_size, &data_section);
	}
	struct UnixFSBlockSizeNode bs;
	bs.block_size = bytes_read;
	ipfs_unixfs_add_blocksize(&bs, data_section);
	data_section->file_size += bytes_read;
	// put the new data back in the data section
	size_t protobuf_size = ipfs_unixfs_protobuf_encode_size(data_section); //delay bytes_size entry
	unsigned char protobuf[protobuf_size];
	ipfs_unixfs_protobuf_encode(data_section, protobuf, protobuf_size, &protobuf_size);
	ipfs_unixfs_free(data_section);
	ipfs_node_set_data(node, protobuf, protobuf_size);
	return 1;
}

/**
 * read the next chunk of bytes, create a node, and add a link to the node in the passed-in node
 * @param file the file handle
 * @param node the node to add to
 * @returns number of bytes read
 */
size_t ipfs_import_chunk(FILE* file, struct Node* parent_node, struct FSRepo* fs_repo, size_t* total_size) {
	unsigned char buffer[MAX_DATA_SIZE];
	size_t bytes_read = fread(buffer, 1, MAX_DATA_SIZE, file);

	// structs used by this method
	struct UnixFS* new_unixfs = NULL;
	struct Node* new_node = NULL;
	struct NodeLink* new_link = NULL;

	// put the file bits into a new UnixFS file
	if (ipfs_unixfs_new(&new_unixfs) == 0)
		return 0;
	new_unixfs->data_type = UNIXFS_FILE;
	new_unixfs->file_size = bytes_read;
	if (ipfs_unixfs_add_data(&buffer[0], bytes_read, new_unixfs) == 0) {
		ipfs_unixfs_free(new_unixfs);
		return 0;
	}
	// protobuf the UnixFS
	size_t protobuf_size = ipfs_unixfs_protobuf_encode_size(new_unixfs);
	if (protobuf_size == 0) {
		ipfs_unixfs_free(new_unixfs);
		return 0;
	}
	unsigned char protobuf[protobuf_size];
	size_t bytes_written = 0;
	if (ipfs_unixfs_protobuf_encode(new_unixfs, protobuf, protobuf_size, &bytes_written) == 0) {
		ipfs_unixfs_free(new_unixfs);
		return 0;
	}

	// we're done with the UnixFS object
	ipfs_unixfs_free(new_unixfs);

	// if there is more to read, create a new node.
	if (bytes_read == MAX_DATA_SIZE) {
		// create a new node
		if (ipfs_node_new_from_data(protobuf, bytes_written, &new_node) == 0) {
			return 0;
		}
		// persist
		size_t size_of_node = 0;
		if (ipfs_merkledag_add(new_node, fs_repo, &size_of_node) == 0) {
			ipfs_node_free(new_node);
			return 0;
		}

		// put link in parent node
		if (ipfs_node_link_create(NULL, new_node->hash, new_node->hash_size, &new_link) == 0) {
			ipfs_node_free(new_node);
			return 0;
		}
		new_link->t_size = size_of_node;
		*total_size += new_link->t_size;
		// NOTE: disposal of this link object happens when the parent is disposed
		if (ipfs_node_add_link(parent_node, new_link) == 0) {
			ipfs_node_free(new_node);
			return 0;
		}
		ipfs_importer_add_filesize_to_data_section(parent_node, bytes_read);
		ipfs_node_free(new_node);
	} else {
		// if there are no existing links, put what we pulled from the file into parent_node
		// otherwise, add it as a link
		if (parent_node->head_link == NULL) {
			ipfs_node_set_data(parent_node, protobuf, bytes_written);
		} else {
			// there are existing links. put the data in a new node, save it, then put the link in parent_node
			// create a new node
			if (ipfs_node_new_from_data(protobuf, bytes_written, &new_node) == 0) {
				return 0;
			}
			// persist
			size_t size_of_node = 0;
			if (ipfs_merkledag_add(new_node, fs_repo, &size_of_node) == 0) {
				ipfs_node_free(new_node);
				return 0;
			}

			// put link in parent node
			if (ipfs_node_link_create(NULL, new_node->hash, new_node->hash_size, &new_link) == 0) {
				ipfs_node_free(new_node);
				return 0;
			}
			new_link->t_size = size_of_node;
			*total_size += new_link->t_size;
			// NOTE: disposal of this link object happens when the parent is disposed
			if (ipfs_node_add_link(parent_node, new_link) == 0) {
				ipfs_node_free(new_node);
				return 0;
			}
			ipfs_importer_add_filesize_to_data_section(parent_node, bytes_read);
			ipfs_node_free(new_node);
		}
		// persist the main node
		ipfs_merkledag_add(parent_node, fs_repo, &bytes_written);
	} // add to parent vs add as link

	return bytes_read;
}

/**
 * Creates a node based on an incoming file or directory
 * NOTE: this can be called recursively for directories
 * @param file_name the file (or directory) to import
 * @param parent_node the root node (has links to others in case this is a large file and is split)
 * @returns true(1) on success
 */
int ipfs_import_file(const char* fileName, struct Node** parent_node, struct FSRepo* fs_repo) {
	/**
	 * NOTE: When this function completes, parent_node will be either:
	 * 1) the complete file, in the case of a small file (<256k-ish)
	 * 2) a node with links to the various pieces of a large file
	 * 3) a node with links to files and directories if 'fileName' is a directory
	 */
	int retVal = 1;
	int bytes_read = MAX_DATA_SIZE;
	size_t total_size = 0;
	unsigned int isDirectory = 0;

	//TODO: determine if this file is actually a directory
	if (isDirectory) {
		// get list of files
		// process each file
		// add file as link to parent_node
	} else {
		// process this file
		FILE* file = fopen(fileName, "rb");
		retVal = ipfs_node_new(parent_node);
		if (retVal == 0)
			return 0;

		// add all nodes (will be called multiple times for large files)
		while ( bytes_read == MAX_DATA_SIZE) {
			bytes_read = ipfs_import_chunk(file, *parent_node, fs_repo, &total_size);
		}
		fclose(file);
	}

	return 1;
}


// a linked list to store filenames
struct FileList {
	char* file_name;
	struct FileList* next;
};

/**
 * called from the command line to import multiple files or directories
 * @param argc the number of arguments
 * @param argv the arguments
 */
int ipfs_import_files(int argc, char** argv) {
	/*
	 * Param 0: ipfs
	 * param 1: add
	 * param 2: -r (optional)
	 * param 3: directoryname
	 */
	struct FSRepo* fs_repo = NULL;
	struct FileList* first = NULL;
	struct FileList* last = NULL;
	int recursive = 0; // false

	// parse the command line
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-r") == 0) {
			recursive = 1;
		} else {
			struct FileList* current = (struct FileList*)malloc(sizeof(struct FileList));
			current->next = NULL;
			current->file_name = argv[i];
			// now wire it in
			if (first == NULL) {
				first = current;
			}
			if (last != NULL) {
				last->next = current;
			}
			// now set last to current
			last = current;
		}
	}

	// open the repo
	int retVal = ipfs_repo_fsrepo_new(NULL, NULL, &fs_repo);
	if (retVal == 0) {
		return 0;
	}
	retVal = ipfs_repo_fsrepo_open(fs_repo);

	// import the file(s)
	struct FileList* current = first;
	while (current != NULL) {
		struct Node* directory_entry = NULL;
		retVal = ipfs_import_file(current->file_name, &directory_entry, fs_repo);

		// give some results to the user
		int buffer_len = 100;
		unsigned char buffer[buffer_len];
		retVal = ipfs_cid_hash_to_base58(directory_entry->hash, directory_entry->hash_size, buffer, buffer_len);
		if (retVal == 0) {
			printf("Unable to generate hash\n");
			ipfs_node_free(directory_entry);
			ipfs_repo_fsrepo_free(fs_repo);
			return 0;
		}
		printf("added %s %s\n", buffer, current->file_name);
		//TODO: cleanup
		ipfs_node_free(directory_entry);
		current = current->next;
	}

	if (fs_repo != NULL)
		ipfs_repo_fsrepo_free(fs_repo);
	// free file list
	current = first;
	while (current != NULL) {
		first = current->next;
		free(current);
		current = first;
	}

	return retVal;
}