<? # vim: syntax=php

# Settings for iqdb-xml.php
$iqdb_host = "localhost";		# Host and
$iqdb_port = 5566;			# port where iqdb server is listening.
$base_dir = "/opt/iqdb/";		# Base directory containing image thumbnails,
					# has "dir" value from $services appended for service thumbs.
$thu_dir = "$base_dir/thu/";		# Directory where generated thumbs are to be saved.
$base_url = "http://localhost/";	# Base URL corresponding to $base_dir.
$maxdim = 2500;				# Maximum allowed image dimensions for thumbnailing.
$thudim = 150;				# Thumbnail dimensions.

# Optional:
# $largedim = 1000;			# Resolution limit above which thumbnail generation will be serialized.
# $largehnd = "serialize_resize";	# Function name to do the serialization, should not return until its turn has come.
					# If it returns an object, that object's destructor will be called when thumbnail
					# generation has completed, so that it may release the lock/semaphore/etc.
# function serialize_resize($size_x, $size_y) { ... }

# Array of database infos for the iqdb server.
# One set of values for each DB.
$services = array(
	0 => "local",			# DBID -> service name
	"local" => array(		# service name ->
		"site" => "localhost",		# service host (also serves as service name for XML output)
		"dir" => "local",		# thumbnail dir (indexed), appended to $base_dir
		"db" => 0,			# DBID
		#"process" => "my_process",	# optional: function to resolve matched image IDs
						# uncomment/set this if you aren't using danbooru style filenames
		# may hold any other keys as required by the front-end.
	),
);

?>
