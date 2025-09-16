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
    // Reload page with ?trace=name
    var url = new URL(window.location.href);
    url.searchParams.set('trace', name);
    window.location.href = url.toString();
  }
});