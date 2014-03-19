<?php

# Implements CurlTransfer, a class to download a URL to
# a temporary file and show the progress, while also
# ensuring that it is an image with max size/dimensions.

class CurlTransfer {

# Process HTTP response header line.
private function get_header($ch,$hdr) {
	if ($ch !== $this->ch) fatal("Bad call of get_header: $ch vs $this->ch");

	# All headers we need have been seen.
	if ($this->stat == 15) return strlen($hdr);

	list($h,$v) = split(": +", chop($hdr));
	#debug("$h = $v; $this->stat");

	# Processing a redirect.
	if ($this->stat == 255) {
		if (substr($h,0,5) === "HTTP/") {
			$this->stat = 0;
		} else {
			if ($h == "Location") debug("Redirected to $v");
			return strlen($hdr);
		}
	}

	# First header line: HTTP status.
	if ($this->stat === 0) {
		list($h,$c,$v) = split(" +", $h, 3);
		if (substr($h,0,5) != "HTTP/" || $c < 200 || $c > 303) { $this->stat = -4; $this->err="$c $v"; return 0; }
		$this->stat = 8;
		if ($c >= 300) {
			$this->redir++;
			$this->stat = 255;
		}

	# else process the important headers.
	} elseif ($h === "Content-Type") {
		$this->content_type = $v;
		if ($v !== "image/jpeg" && $v !== "image/jpg" && $v !== "image/png" && $v != "image/gif") { $this->stat = -1; return 0; }
		$this->stat |= 1;
	} elseif ($h === "Content-Length") {
		if ($this->showed)
			echo sprintf($this->size_delayed, ($v + 512) / 1024);
		$this->size = $v;
		if ($v < 128) { $this->stat = -2; return 0; }
		if ($v > $this->maxsize) { $this->stat = -3; return 0; }
		$this->stat |= 2;
	} elseif ($h === "X-Cache") {
		$this->cache = $v;
		$this->stat |= 4;
	}
	return strlen($hdr);
}

private function show_size($prog) {
	$this->showed = 1;
	echo sprintf($this->size ? $this->size_known : $this->size_unknown, ($this->size + 512) / 1024, $prog);
	if (!$this->size) $this->size = 1;
}

# Display progress message.
function progress($prog) {
	if ($this->showed)
		echo sprintf($this->update, $prog, $this->suff);
	else
		$this->show_size($prog);
}

# Receive HTTP data.
private function http_progress($ch,$data) {
	if ($ch !== $this->ch) fatal("Bad call of http_progress: $ch vs $this->ch");

	if ($data) {
		$this->complete += strlen($data);
		if ($this->complete > $this->maxsize) { $this->stat = -3; $this->size = -$this->complete; return 0; }
		$this->suff = '';
	}

	# No more than once a second, display progress update.
	if ($this->show_progress && (microtime_float() >= $this->last + 1) || !$data) {
		if ($this->size <= 1)
			$this->progress(sprintf("%d KB", $this->complete / 1024));
		else
			$this->progress(sprintf("%d%%", $this->complete * 100 / $this->size));
		ob_flush(); flush();
		$this->last = microtime_float();
		if (!$data) return;
	}

	# Write data, and after $this->chk bytes try if it's an image and then check its dimensions.
	$out = fwrite($this->file->fd, $data);
	if (($this->complete > $this->chk) && ($this->complete - strlen($data) <= $this->chk)) {
		#debug("Size is now $this->complete after writing ".strlen($data));
		@$size = getimagesize($this->file->name);
		#debug("At $this->complete bytes, image info ".($size?join(":", $size):"unknown"));
		if (!$size) {
			#$this->stat = -1; $this->err = "unknown data"; return 0;
			$this->chk += $this->complete;
		} else if ($size[0] > $this->maxdim || $size[1] > $this->maxdim) {
			#debug("Image too large! ".join(":", $size));
			$this->stat = -5;
			$this->err = array("", $size[0]." x ".$size[1]);
			return 0;
		}
		#debug("Image OK!");
	}
	return $out;
}

# Execute curl handle asynchronously, to be
# able to show progress even when it stalls.
function execute() {
	$mh = curl_multi_init();
	curl_multi_add_handle($mh, $this->ch);
	$active = false;
	$this->wait = 1;
	$this->last = microtime_float();
	while (true) {
		while (curl_multi_exec($mh, $active) == CURLM_CALL_MULTI_PERFORM);
		if (!$active) break;

		# Wait at most 2 seconds for more data, else show it stalled.
		$active = curl_multi_select($mh, $this->wait);
		$this->wait = 2;
		if (!$active && $this->show_progress) {
			$this->suff = $this->suff ? "$this->suff." : " stalled...";
			$this->http_progress($this->ch, false);
		}
	}
	$ret = -1;
	while (($msg = curl_multi_info_read($mh))) {
		#debug("multi msg=".$msg["msg"]." result=".$msg["result"]);
		if ($msg["msg"] === CURLMSG_DONE)
			$ret = ($msg["result"] == CURLE_OK);
	}
	curl_multi_remove_handle($mh, $this->ch);
	curl_multi_close($mh);
	$this->file->close();
	if ($this->stat >= 0) $this->error = curl_error($this->ch);
	return $ret === true;
}

# Set a CURL option.
function setopt($opt, $val) { return curl_setopt($this->ch, $opt, $val); }

# Make a curl handle and set the options we need.
function __construct($url) {
	$this->stat = 0;
	$this->ch = curl_init($url);
	if (!$this->ch) { $this->error = "NoMemory"; return; }

	$this->file = TmpFile::make("/tmp", "sim_url_", "w");
	if (!$this->file) { $this->error = "NoTempfile"; return; };

	$this->setopt(CURLOPT_HEADERFUNCTION, array($this, "get_header"));
	$this->setopt(CURLOPT_WRITEFUNCTION, array($this, "http_progress"));
	#$this->setopt(CURLOPT_FAILONERROR, true);
	$this->setopt(CURLOPT_AUTOREFERER, true);
	$this->setopt(CURLOPT_FOLLOWLOCATION, true); 
	$this->setopt(CURLOPT_MAXREDIRS, 5); 
	$this->setopt(CURLOPT_HTTPHEADER, array("Pragma: "));
	$this->setopt(CURLOPT_USERAGENT, "iqdb/0.1 (+http://iqdb.org/)");
	//$this->setopt(CURLOPT_FILE, $this->file->fd);
}

}

?>
