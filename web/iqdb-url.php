<?php	# vim: syntax=php

# Sample code for using iqdb-url.inc

include_once("iqdb-php.inc");	# for TmpFile
include_once("iqdb-url.inc");	# for CurlTransfer

# Retrieve $url (call it $org in error message, e.g. the filename part only).
# If not $quiet then regularly output status update.
function get_url($url, $org, $quiet) {
	global $maxdim,$maxsize;

	$curl = new CurlTransfer($url);
	if ($curl->error) {
		if ($curl->error == "NoMemory")
			showerr("Out of memory");
		elseif ($curl->error == "NoTempfile")
			showerr("Can't make temp file.");
		else
			showerr("Unknown error");
		return false;
	}

	$curl->chk = 32*1024;		# every this many bytes, file is checked if it's really an image
	$curl->maxsize = $maxsize;	# maximum allowed file size
	$curl->maxdim = $maxdim;	# maximum image dimension
	#$curl->setopt(CURLOPT_PROXY, "http://localhost:3128/");

	if (!$quiet) {
		$curl->show_progress = 1;

		# Shown when Content-Length header is received in under 1s, arg1=size in KB, arg2=progress so far
		$curl->size_known = " (%d KB)... <span id='urlstat'>%s</span></div>\n";

		# Shown when Content-Length header is not received within 1s, same args
		$curl->size_unknown = " (<span id='urlsize'>?</span> KB)... <span id='urlstat'>%2\$s</span></div>\n";

		# Show when Content-Length header is received late, arg1=size in KB
		$curl->size_delayed = "<script type='text/javascript'>url_size('%d');</script>\n";

		# Regular updates output every 1-2s, arg1=progress so far, arg2=additional info
		$curl->update = "<script type='text/javascript'>progress('%s','%s');</script>\n";
	}

	$reqstart = microtime_float();
	$res = $curl->execute();
	if (!$quiet) {
		$redirs = $curl->redir == 1 ? " [Followed 1 redirect]" : ($curl->redir ? sprintf(" [Followed %d redirects]", $curl->redir) : "");
		$done = $res ? sprintf("OK, %.1f seconds.$redirs", microtime_float() - $reqstart) : "failed";
		$curl->progress($done);
	}
	if (!$res) {
		debug("Failed... $curl->stat $curl->error");
		if ($curl->stat == 255) {
			showerr("Transfer failed... too many redirections.");
		} elseif ($curl->stat >= 0) {
			dolog("URL failed: $url");
			showerr(sprintf("Transfer failed: %s", $curl->error));
		} elseif ($curl->stat == -1) {
			showerr(sprintf("Failed: %s has unsupported type %s",  htmlentities($org), $curl->content_type), "Check the URL");
		} elseif ($curl->stat == -2) {
			showerr("No data received.");
		} elseif ($curl->stat == -3) {
			if ($curl->size < 0)
				showerr(sprintf("Aborted %s after receiving %d KB: too large.",  htmlentities($org), round(-$curl->size / 1024)));
			else
				showerr(sprintf("%s is too large: %d KB",  htmlentities($org), round($curl->size / 1024)));
		} elseif ($curl->stat == -4) {
			dolog("$curl->err: $url");
			showerr(sprintf("Request failed: %s", $curl->err));
		} elseif ($curl->stat == -5) {
			$curl->err[0] = htmlentities($org);
			#debug(join(":",$curl->err));
			showerr(vsprintf("Image %s too large (%d x %d)", $curl->err), "Try downloading a thumbnail version.");
		}
		return false;
	}

	return $curl->file;
}

?>
