/* global Pebble, navigator */
(function () {
  'use strict';

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
  });

  // watch sends an empty dict as a "refresh" ping on light-on
  Pebble.addEventListener('appmessage', function() {
    getAndSend();
  });
})();
