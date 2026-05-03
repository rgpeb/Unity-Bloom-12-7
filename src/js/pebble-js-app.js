/* global Pebble, navigator */
(function () {
  'use strict';
  var STORAGE_KEY = 'unitybloom_time24';

  function cToF(c) { return Math.round((c * 9/5) + 32); }

  function send(tempF, uvi) {
    Pebble.sendAppMessage(
      { 'TEMP': tempF, 'UVI': uvi },
      function(){ /* ok */ },
      function(){ /* fail */ }
    );
  }

  function fetchWeather(lat, lon) {
    var url = 'https://api.open-meteo.com/v1/forecast'
      + '?latitude=' + lat
      + '&longitude=' + lon
      + '&current=temperature_2m,uv_index';

    var xhr = new XMLHttpRequest();
    xhr.onload = function() {
      try {
        var j = JSON.parse(xhr.responseText);
        var curr = j && j.current;
        if (!curr) return;

        var tempC = curr.temperature_2m;
        var uvi   = curr.uv_index;
        send(cToF(tempC), Math.round(uvi));
      } catch (e) {}
    };
    xhr.open('GET', url);
    xhr.send();
  }

  function getAndSend() {
    if (!navigator || !navigator.geolocation) { return; }
    navigator.geolocation.getCurrentPosition(function(pos){
      fetchWeather(pos.coords.latitude, pos.coords.longitude);
    }, function(){}, { timeout: 10000, maximumAge: 600000 });
  }

  Pebble.addEventListener('ready', function() {
    getAndSend();
    setInterval(getAndSend, 30 * 60 * 1000);
    var saved = localStorage.getItem(STORAGE_KEY);
    if (saved !== null) {
      Pebble.sendAppMessage({ 'TIME24': saved === '1' ? 1 : 0 });
    }
  });

  // watch sends an empty dict as a "refresh" ping on light-on
  Pebble.addEventListener('appmessage', function() {
    getAndSend();
  });

  Pebble.addEventListener('showConfiguration', function() {
    var saved = localStorage.getItem(STORAGE_KEY);
    var checked = (saved === null) ? 0 : (saved === '1' ? 1 : 0);
    var html = '<html><body style=\"font-family:sans-serif;padding:20px;\">'
      + '<h3>Unity Bloom Settings</h3>'
      + '<label><input id=\"t24\" type=\"checkbox\" ' + (checked ? 'checked' : '') + '> Use 24-hour time</label>'
      + '<br><br><button onclick=\"save()\">Save</button>'
      + '<script>function save(){var v=document.getElementById(\"t24\").checked?1:0;'
      + 'location.href=\"pebblejs://close#\"+encodeURIComponent(JSON.stringify({time24:v}));}</script>'
      + '</body></html>';
    Pebble.openURL('data:text/html,' + encodeURIComponent(html));
  });

  Pebble.addEventListener('webviewclosed', function(e) {
    if (!e || !e.response) return;
    try {
      var cfg = JSON.parse(decodeURIComponent(e.response));
      var time24 = cfg && cfg.time24 ? 1 : 0;
      localStorage.setItem(STORAGE_KEY, time24 ? '1' : '0');
      Pebble.sendAppMessage({ 'TIME24': time24 });
    } catch (err) {}
  });
})();
