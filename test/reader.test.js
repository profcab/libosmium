var osmium = require('../');
var fs = require('fs');
var assert = require('assert');

describe('reader', function() {

    it('should throw when Reader called as function', function(done) {
        assert.throws(function() {
            var reader = osmium.Reader("foo");
        }, Error);
        done();
    });

    it('should not hang when apply() is called twice on reader', function(done) {
        var file = new osmium.File(__dirname + "/data/winthrop.osm");
        var reader = new osmium.Reader(file);
        var handler = new osmium.Handler(); 
        osmium.apply(reader, handler);

        assert.throws(function() {
            osmium.apply(reader, handler);
        }, Error);
        done();
    });

    it('should be able to create an osmium.Reader and access header', function(done) {
        var file = new osmium.File(__dirname + "/data/winthrop.osm");
        var reader = new osmium.Reader(file, {});
        var header = reader.header();
        assert.equal(header.generator, 'CGImap 0.2.0');

        var bounds = header.bounds[0];
        assert.ok(Math.abs(bounds.left()   - (-120.2024)) < .000000001);
        assert.ok(Math.abs(bounds.bottom() - (  48.4636)) < .000000001);
        assert.ok(Math.abs(bounds.right()  - (-120.1569)) < .000000001);
        assert.ok(Math.abs(bounds.top()    - (  48.4869)) < .000000001);
        reader.close();

        done();
    });

    it('should be able to call apply() with an osmium.Reader and a handler', function(done) {
        var reader = new osmium.Reader(__dirname + "/data/winthrop.osm");
        var handler = new osmium.Handler();

        osmium.apply(reader, handler);
        done();
    });

    it('should be able to create osmium.File with node.Buffer and read from it', function(done) {
        var buffer = fs.readFileSync(__dirname + "/data/winthrop.osm");
        assert.equal(buffer.length, 359898);

        var file = new osmium.File(buffer, "osm");
        assert.ok(file);

        var reader = new osmium.Reader(file);
        assert.ok(reader);

        var handler = new osmium.Handler();

        var count = 0;
        handler.on('node', function(node) {
            if (count == 0) {
                assert.equal(node.id, 50031066);
                done();
            }
            count++;
        });

        osmium.apply(reader, handler);
    });

});
