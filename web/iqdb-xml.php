<?php

# Put your server settings in iqdb-opt.inc
# Set up a cronjob to regularly delete generated thumbnails.

# Upload file via POST with the following form fields:
# <input type="file" name="file">
# <input type="text" name="service"> or several <input type="checkbox" name="service[]" value="0">...
# <input type="checkbox" name="forcegray"> (optional)

# --------------

include('iqdb-opt.inc');

include("iqdb-php.inc");

header("Content-type: application/xml; charset=utf-8");
echo "<?xml version='1.0' encoding='UTF-8'?>\n";

function error($text) {
	echo '<error message="'.htmlentities($text, ENT_QUOTES).'" info=""></error>';
	exit;
}
# for debugging problems
function debug($txt) {
	echo "<!-- $txt -->\n";
}

# sample processing function if the filenames aren't in the default danbooru style
function my_process($arg) {
	global $services;
	# do something with the image ID in $arg["id"]
	# also set these:
	$arg["eng"] = "";	// this will go in the name="" field of the XML output
	$arg["tag"] = $services[0];	// DB number, or anything that maps to the service name in $services
	return $arg;
}

foreach (array_keys($services) as $srv)
	if (is_array($services[$srv]))
		$services[$srv]["fullpath"] = "$base_dir/".$services[$srv]["dir"];

if ($_FILES['file']['error']) error("Upload error code ".$_FILES['file']['error']);

$thumb = make_thumb($_FILES['file']['tmp_name'], $_FILES['file']['org']);
if ($thumb['err']) error("Thumbnail error code ".$thumb['err']);

$res = request_match($thumb["name"], $_POST["service"], array("forcegray" => $_POST["forcegray"]));
if ($res['err']) error($res['err']);
if ($res['fatal']) error("Fatal: ".$res['err']);

$thres = process_match($res["match"]);
if ($thres['err']) error($thres['err']);

produce_xml($res["match"], $thumb, $thres);

?>
