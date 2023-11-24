function measure_request1() {
    fetch('/api/measure/start', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        }
    });
}

function measure_request2() {
    fetch('/api/measure/stop', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        }
    });
}
