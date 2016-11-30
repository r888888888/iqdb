<?php

# The functions microtime_float, thumb_fname, make_thumb and request_srv may
# be defined by your code before including this file, if you need to do their
# work a little differently. Then you can still use the other functions here.

# Connect to iqdb server, send it a command and parse the replies.
if (!function_exists("request_srv")) {
function request_srv($line) {
	global $iqdb_host, $iqdb_port, $iqdb_restart_cmd;

	for ($try = 0; $try < 3; $try++) {
		@$fp = fsockopen($iqdb_host, $iqdb_port, $errno, $errstr, 3);
		if ($fp) break;

		debug("Can't connect, errno=$errno errstr=$errstr try=$try"); flush();
		if ($errno == 111 /* ECONNREFUSED */ && $try == 2 && $iqdb_restart_cmd) {
			debug("Trying to restart iqdb: $iqdb_restart_cmd");
			exec($iqdb_restart_cmd, $out);
			debug("Output:\n".join("\n",$out),"\n");
		}
		sleep(2);
	}

	if (!$fp) 
		return array("fatal" => "Whoops, can't connect to database ;_;", "fatal2" => "Go to <a href='irc://irc.rizon.net/iqdb'>#iqdb.rizon.net</a> to rant.");

	#debug("Sending: $line\n");
	fwrite($fp, "$line\n");
	fflush($fp);

	$response = "";
	while (!feof($fp))
		$response.=fread($fp, 16384);
	fclose($fp);

	return parse_response($response);
}
}

function parse_response($lines) {
	$res = array("info" => array(), "values" => array(), "results" => array());

	#debug("got $lines");
	foreach (explode("\n", $lines) as $line) {
		#debug("got $line");
		list($code,$rest) = explode(" ", chop($line), 2);
		#debug("code=$code rest=$rest");
		switch($code) {
			case "":  break;
			case 000: break;
			case 100: array_push($res["info"], $rest);
				  break;
			case 101: list($var,$val) = explode("=", $rest, 2);
				  $res["values"][$var] += $val;
				  break;
			case 200: array_push($res["results"], "-1 $rest");
				  break;
			case 201: array_push($res["results"], $rest);
				  break;
			case 300:
			case 301:
			case 302: $res["err"] = $rest;
				  break;
			default:
				  #debug("$line code $code rest $rest");
				  return array("fatal" => "Unsupported result code $code $rest");
		}
	}

	#debug($res["results"]);
	return $res;
}

# For stats on how long the various search steps take.
if (!function_exists("microtime_float")) {
function microtime_float() {
	list($usec, $sec) = explode(" ", microtime());
	return ((float)$usec + (float)$sec);
}
}

# Ask iqdb server for images most similar to given file.
function request_match($file, $querysrv, $options = array()) {
	global $services;
	global $last_request_file;
	
	$last_request_file = $file;

	$start = microtime_float();
	$res = array("results" => array());
	$numres = $options["numres"];
	if (!$numres) $numres = 16;
	$flags = $options["forcegray"] ? 2 : 0;
	$queryopt = "";
	if ($options["mask_and"])
		$queryopt=sprintf("query_opt mask %d %d\n", $options["mask_and"], $options["mask_xor"]);
	if (!is_array($querysrv) && array_key_exists("query_db", $services[$services[$querysrv]]))
		$querysrv = $services[$services[$querysrv]]["query_db"];
	if (is_array($querysrv)) {
		$res = "";
		$query = "";
		foreach ($querysrv as $db) {
			if (!array_key_exists($db, $services)) return array('err' => "Service $db unknown.");
			$srv=$services[$services[$db]];
			$rflags = $flags;
			if ($srv["uniqueset"]) $rflags |= 8;
			$db=$srv["db"];
			$res.="count $db\n";
			if ($query) $query.=' +';
			$query.=" $db $rflags $numres";
		}
		$query = $res."${queryopt}multi_query$query $file\ndone\n";
		#debug("Doing multi query:\n$query");
		$res = request_srv($query);
		#debug(print_r($res, TRUE));
		if ($res["results"]) $res["results"] = array_slice($res["results"], 0, $numres);
		#debug(print_r($res, TRUE));
	} else {
		$srv=$services[$services[$querysrv]];
		$db = $srv["db"];
		if ($srv["uniqueset"]) $flags |= 8;
		$query = "count $db\n${queryopt}query $db $flags $numres $file\ndone\n";
		#debug("Doing simple query for $querysrv:\n$query");
		$res = request_srv($query);
	}
	if ($res["fatal"]) return $res;
	$time = microtime_float() - $start;
	if ($res["values"]["time"]) $time += $res["values"]["time"];
	$start = microtime_float();
	$result = array("values" => $res["values"], "match" => array());

	$args = array();
	$resnum = -1;
	$out = array();
	foreach ($res["results"] as $line) {
		$resnum++;
		list($dbid,$id,$sim,$width,$height) = explode(" ", $line);
		if (!$width) $width=-1;
		if (!$height) $height=-1;
		if ($dbid == -1) $dbid = $services[$services[$querysrv]]["db"];
		#debug("Line: $line  DB: $dbid -> ".$services[$dbid]." ID/sim/w/h $id $sim $width $height");

		$srv = $services[$services[$dbid]];
		if ($srv["process"]) {
			#debug("Will process dbid $dbid id $id");
			$arg = array("dbid" => $dbid, "id" => hexdec($id), "sim" => $sim, "width" => $width, "height" => $height, "file" => $file, "resnum" => $resnum, "ok" => 0);
			$args[$dbid][] = $arg;
			continue;
		}
		#debug("Direct dbid $dbid id $id");

		$srvdir = $srv["fullpath"];
		$pref = substr($id,0,1)."/".substr($id,1,1)."/".substr($id,2,1);
		#debug("$id -> $srvdir/$pref");
		$files = glob("$srvdir/$pref/$id*");
		if (count($files) > 1) continue;
		if (count($files) == 0) $files = glob("$srvdir/xx/$id-*");
		#debug("result: $line. ID:$id Pref:$pref sim:$sim files:".join(";",$files));
		if (count($files) != 1) continue;
		if (!preg_match("#/(".$srv["dir"]."/[0-9a-fx\/]+/([0-9a-f-]+)\.jpg)$#", $files[0], $eres)) continue;
		$out[$resnum] = array('sim' => $sim, 'md5' => substr($eres[2],-32), 'fname' => $eres[1], 'tag' => $srv["dir"], 'width' => $width, 'height' => $height);
	}
	foreach ($args as &$arg) {
		$tag = $services[$arg[0]["dbid"]];
		#debug("Querying $tag for ".count($arg)." IDs");
		$matches = $services[$services[$arg[0]["dbid"]]]["process"]($arg);
		if (!is_array($matches)) continue;
		if ($matches["err"]) return array('err' => $matches["err"]);
		$argnums = array();
		for ($argnum = 0; $argnum < count($arg); $argnum++) $argnums["".$arg[$argnum]["id"]] = $argnum;
		#$debout="";foreach($argnums as $key => $value)$debout.=" $key:$value";debug("Using arg inds$debout");
		foreach ($matches as &$match) {
			$argnum = $argnums[$match["id"]];
			#debug("$tag: Result for id ".$match['id']." is arg #$argnum and result #".$arg[$argnum]["resnum"]." i.e. id ".$arg[$argnum]["id"]." sim=".$arg[$argnum]["sim"]);
			$match['tag'] = $tag;
			$match['sim'] = $arg[$argnum]["sim"];
			$out[$arg[$argnum]["resnum"]] = $match;
			$arg[$argnum]["ok"] = 1;
		}
		foreach ($arg as $res) {
			if (!$res["ok"] && $res['dbid']) {	# ignore for haruhi DB
				$lf = @fopen("../faulty-id.log", "a");
				if ($lf) {
					fputs($lf, "tag=$tag dbid=".$res['dbid']." id=".$res['id']." MISSING_IN_DB\n");
					fclose($lf);
				}
				#debug("DB $tag result not OK: ".print_r($res, TRUE));
			}
		}
	}
	for($i=0; $i<=$resnum; $i++) {
		if ($out[$i]) {
			$result["match"][] = $out[$i];
		} else {
			#debug("No DB data for result $i: ".print_r($matches[$i], TRUE));
		}
	}
	#$debout="";foreach ($result["match"] as $key => $val)$debout.=" $key:".$val["id"]."=".$val["sim"];debug("Sims:$debout");
	$dbtime = microtime_float() - $start;
	#debug("Got ".count($res["results"])." results parsed into ".count($result["match"])." matches.");
	if (count($res["results"]) && !count($result["match"])) return array('err' => "Thumbnails not found or search returned no results.");
	if (!count($result["match"])) return array('err' => "Database error: ".($res["err"]?$res["err"]:"No matches returned."));
	$result["stats"] = sprintf("query %.3fs db %.3fs", $time, $dbtime);
	return $result;
}

# Process a result set: find if there is a relevant match and what the threshold is.
function process_match($match, $ignoremd5 = false, $ignoretag = '') {
	if(!is_array($match)) return array("err" => "Database returned nothing.");
	$sum = 0;
	$count = 0;
	$sqsum = 0;
	foreach ($match as $page) {
		if ($ignoremd5 && $page["md5"] === $ignoremd5 && $page["tag"] === $ignoretag) continue;
		$sum += $page['sim'];
		$sqsum += $page['sim'] * $page['sim'];
		$count++;
		#debug("Checking sim=".$page['sim'].", sum=$sum sqsum=$sqsum count=$count");
	}
	if(!$count) return array("err" => "Database returned nothing.");
	$avg = $sum/$count;
	$stddev = sqrt($sqsum/$count - $avg*$avg);
	$minrel = $avg + $stddev/2;
	$thres = array("avg" => $avg, "stddev" => $stddev, "numrel" => 0);

	if ($stddev < 5 && $match[0]['sim'] < 80) {
		$thres["minsim"] = $minrel;
		$minrel = 70;
	} else {
		foreach ($match as $page) {
			#debug("Checking sim=".$page['sim'].", sum=$sum sqsum=$sqsum avg=$avg stddev=$stddev minrel=$minrel count=$count");
			if ($page['sim'] < $minrel) break;
			$thres["numrel"]++;
			if ($ignoremd5 && $page["md5"] === $ignoremd5 && $page["tag"] === $ignoretag) continue;
			if (!--$count) break;
			$sum -= $page['sim'];
			$sqsum -= $page['sim'] * $page['sim'];
			$avg = $sum/$count;
			$stddev = sqrt($sqsum/$count - $avg*$avg);
			if ($stddev < 5) continue;
			$minrel = $avg + $stddev/2;
		}
	}
	#debug("avg=".$thres["avg"]." stddev=".$thres["stddev"]." minrel=$minrel numrel=".$thres["numrel"]);
	$thres["minrel"] = $minrel;
	return $thres;
}

function resample_thumb($image, $fname, $fqual, $thu_x, $thu_y, $size_x, $size_y) {
	$thu = @imagecreatetruecolor($thu_x, $thu_y);
	if (!$thu) return false;

	$res = (($bg = @imagecolorallocate($thu, 255, 255, 255)) &&
		 @imagefilledrectangle($thu, 0, 0, $thu_x, $thu_y, $bg) &&
		 @imagecopyresampled($thu, $image, 0, 0, 0, 0, $thu_x, $thu_y, $size_x, $size_y) &&
		 @imagejpeg($thu, $fname, $fqual));
	imagedestroy($thu);
	return $res;
}

if (!function_exists("thumb_fname")) {
function thumb_fname(&$thumb, $pref, $ext = ".jpg") {
	global $thu_dir;
	for ($count = 0; $count < 32; $count++) {
		$seed = substr(md5($seed.microtime().posix_getpid()), 0, 8);
		$thumb["seed"] = $seed;
		$thumb["fname"] = "$pref$seed$ext";
		$thumb["name"] = "$thu_dir/".$thumb["fname"];
		@$fh = fopen($thumb["name"], "x");
		#debug(array("thu $count: $thuname", $fh ? "success" : "failed"));
		if ($fh) return $fh;
	};
	return false;
}
}

# Save JPEG thumbnail of $org in $img. Return thumb info or error.
if (!function_exists("make_thumb")) {
function make_thumb($img, $org) {
	global $thudim, $maxdim, $largedim, $largehnd;

	@$size = getimagesize($img);
	if (!$size) 
		return array("err" => "NotImage", "errarg" => array(htmlentities($org)));

	$thumb = array("org" => $org, "size_x" => $size[0], "size_y" => $size[1], "size_kb" => filesize($img)/1024);
	#debug("size ".join(":", $size));
	if ($size[0] > $maxdim || $size[1] > $maxdim)
		return array("err" => "TooBig", "errarg" => array(htmlentities($org), $size[0], $size[1]), "erralt" => "ResizeIt");
	if ($largehnd && ($size[0] > $largedim || $size[1] > $largedim))
		$serialize = $largehnd($size[0], $size[1]);
	else
		$serialize = false;

	if ($size[2] == IMAGETYPE_GIF)
		@$image = imagecreatefromgif($img);
	else if ($size[2] == IMAGETYPE_JPEG)
		@$image = imagecreatefromjpeg($img);
	else if ($size[2] == IMAGETYPE_PNG)
		@$image = imagecreatefrompng($img);
	else
		return array("err" => "NotSupp", "errarg" => array(htmlentities($org), image_type_to_mime_type($size[2])));

	if (!$image)
		return array("err" => "NotImage", "errarg" => array(htmlentities($org)));

	$fh = thumb_fname($thumb, "thu_");
	if (!$fh) return array("err" => "ThumbErr");
	fclose($fh);

	/* Create thumbnail in PHP */
	if ($size[0] > $size[1]) {
		$thu_x = $thudim;
		$thu_y = floor($size[1] * $thudim / $size[0]);
	} else {
		$thu_y = $thudim;
		$thu_x = floor($size[0] * $thudim / $size[1]);
	}
	$thumb["thu_x"] = $thu_x;
	$thumb["thu_y"] = $thu_y;
	#debug("thumbnail $thuname: $thu_x $thu_y from $size[0] $size[1]");

	$start = microtime_float();
	$res = resample_thumb($image, $thumb["name"], 95, $thu_x, $thu_y, $size[0], $size[1]);

	# Make 128x128 for the DB. Store TmpFile object in $thumb, which causes
	# the file to get deleted when $thumb is destructed.
	### Actually not really useful, merely doubles the time to generate thumbs.
	#if ($res &&
	#    ($thumb["_dbthu"] = TmpFile::make("/tmp", "db_thu_")) &&
	#    resample_thumb($image, $thumb["_dbthu"]->name, 98, 128, 128, $size[0], $size[1])) {
	#	$thumb["dbfname"] = $thumb["_dbthu"]->name;
	#	chmod($thumb["dbfname"], 0644);
	#	$thumb["_dbthu"]->close();
	#}

	@imagedestroy($image);
	$end = microtime_float();

	// Call destructor if it was an object.
	$serialize = false;
	if (!$res) return array("err" => "ThumbErr");
	$thumb["stats"] = sprintf(" thu %.3fs", $end-$start);
	return $thumb;
}
}

# Output list of matches as XML.
function produce_xml($match, $thumb, $thres) {
	global $services;

	if ($thumb["cname"])
		$thumburl="/".$thumb['cname'];
	else if (is_array($thumb["fname"]))
		$thumburl="/thu/".$thumb["fname"][0];
	else
		$thumburl="/thu/".$thumb["fname"];
	echo "<matches preview='//iqdb.org$thumburl' threshold='".$thres["minrel"]."'>\n";
	foreach ($match as $page) {
		if ($page['md5']) $m = '" md5="'.$page['md5'].'"';
		else $m = '" id="'.$page['id'].'" name="'.htmlentities($page['eng']).'"';
		if ($page['width']) $m .= ' width="'.$page['width'].'" height="'.$page['height'].'"';
		if ($page['fname'])
			if (is_array($page['fname']))
				$m .= " preview='//iqdb.org/".htmlentities($page['fname'][0], ENT_QUOTES)."'";
			else
				$m .= " preview='//iqdb.org/".htmlentities($page['fname'], ENT_QUOTES)."'";
		echo '<match sim="'.$page['sim'].'" service="'.$services[$page['tag']]["site"].$m;
		if ($page['post'])
			echo ">".$page['post']."</match>\n";
		else
			echo "/>\n";
	}
	echo "</matches>";
}

# Class that automatically cleans up a temp file in its
# destructor, so it gets cleaned up even if the script is
# aborted for example.
class TmpFile {
	function make($path, $pref, $mode = "rw") {
		$tmp = new TmpFile();
		$tmp->name = tempnam($path, $pref);
		#debug("Got tmp $path $pref name ".$tmp->name);
		if (!$tmp->name) return false;
		$tmp->fd = fopen($tmp->name, $mode);
		if (!$tmp->fd) return false;
		#debug("Got FD ".$tmp->fd);
		return $tmp;
	}

	function close() {
		#debug("Closing tmpfile ".$this->name." FD ".$this->fd);
		if ($this->fd) fclose($this->fd);
		$this->fd = false;
	}

	private function __construct() { }

	function __destruct() {
		#debug("Destroying tmp file, name ".$this->name." fd ".$this->fd);
		$this->close();
		if ($this->name) unlink($this->name);
		$this->name = false;
	}
}

?>
