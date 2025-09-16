mergeInto(LibraryManager.library, {
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
    // Call backend to set embed.tracy, then reload
    fetch('/set-embed-tracy/' + encodeURIComponent(name), { method: 'GET' })
      .then(r => {
        if (!r.ok) throw new Error('Failed to set embed.tracy');
        location.reload();
      })
      .catch(e => {
        alert('Failed to load trace: ' + e);
      });
  }
});