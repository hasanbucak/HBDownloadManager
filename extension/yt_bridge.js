// HB Download Manager — YouTube Main World Bridge
// Reads real YouTube player adaptive stream formats & sizes from the page
(() => {
  'use strict';

  function extractAndSend() {
    try {
      let pr = null;
      const player = document.getElementById('movie_player');
      if (player && typeof player.getPlayerResponse === 'function') {
        pr = player.getPlayerResponse();
      }
      if (!pr && window.ytInitialPlayerResponse) {
        pr = window.ytInitialPlayerResponse;
      }
      if (pr && pr.streamingData && Array.isArray(pr.streamingData.adaptiveFormats) && pr.streamingData.adaptiveFormats.length > 0) {
        window.dispatchEvent(new CustomEvent('HBDM_YT_STREAM_DATA', {
          detail: {
            videoId: pr.videoDetails ? pr.videoDetails.videoId : '',
            title: pr.videoDetails ? pr.videoDetails.title : '',
            adaptiveFormats: pr.streamingData.adaptiveFormats
          }
        }));
        return true;
      }
    } catch (e) {}
    return false;
  }

  function scheduleExtraction() {
    let attempts = 0;
    const iv = setInterval(() => {
      attempts++;
      if (extractAndSend() || attempts >= 10) {
        clearInterval(iv);
      }
    }, 500);
  }

  window.addEventListener('HBDM_REQUEST_YT_DATA', extractAndSend);
  window.addEventListener('yt-navigate-finish', () => scheduleExtraction());
  window.addEventListener('load', () => scheduleExtraction());

  scheduleExtraction();
})();
