// Download a trace file from the server
function downloadTrace(ptr) {
  const filename = UTF8ToString(ptr);
  console.info('C++');
  const url = '/traces/' + encodeURIComponent(filename);
  console.log('[Tracy] Downloading trace:', filename, 'from', url);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
}

// Delete a trace file from the server
function deleteTrace(ptr) {
  const filename = UTF8ToString(ptr);
  if (!confirm('Are you sure you want to delete "' + filename + '"?')) return;
  const url = '/traces/' + encodeURIComponent(filename);
  fetch(url, { method: 'DELETE' })
    .then(response => {
      if (response.ok) {
        alert('Deleted: ' + filename);
        // If the current trace is the one deleted, remove the query param and reload
        const urlObj = new URL(window.location.href);
        if (urlObj.searchParams.get('trace') === filename) {
          urlObj.searchParams.delete('trace');
          window.location.href = urlObj.toString();
        } else {
          location.reload();
        }
      } else {
        response.text().then(text => alert('Delete failed: ' + text));
      }
    })
    .catch(err => alert('Delete failed: ' + err));
}

mergeInto(LibraryManager.library, {
  downloadTrace: downloadTrace,
  deleteTrace: deleteTrace,
  getTraceCount: function() {
    return typeof tracyTracesList !== 'undefined' ? tracyTracesList.length|0 : 0;
  },
  getTraceName: function(idx) {
    if (typeof tracyTracesList === 'undefined' || idx < 0 || idx >= tracyTracesList.length) return 0;
    var str = tracyTracesList[idx];
    var len = lengthBytesUTF8(str) + 1;
    var ptr = stackAlloc(len);
    stringToUTF8(str, ptr, len);
    return ptr;
  },
  fetchAndWriteTrace: function(idx) {
    if (typeof tracyTracesList === 'undefined' || idx < 0 || idx >= tracyTracesList.length) return;
    var name = tracyTracesList[idx];
    // Reload page with ?trace=name
    var url = new URL(window.location.href);
    url.searchParams.set('trace', name);
    window.location.href = url.toString();
  }
});
