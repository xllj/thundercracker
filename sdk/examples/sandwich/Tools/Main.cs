using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace MapTool {

  public static class Program {

    public static void Main(string[] args) {

      while(!File.Exists("Makefile")) {
        Directory.SetCurrentDirectory("..");
        if (Directory.GetCurrentDirectory().Length < 5) {
          Console.WriteLine("ERROR: Could not find game folder");
          return;
        }
      }
      if (args.Length > 0) {
        switch(args[0]) {
          case "-gen_lua":
            using (var writer = new StreamWriter("gen_assets.lua")) {
              WriteLua(writer);
            }
            break;
          case "-gen_cxx":
            using (var hWriter = new StreamWriter("gen_mapdata.h")) {
              using (var sWriter = new StreamWriter("gen_mapdata.cpp")) {
                WriteCxx(hWriter, sWriter);
              }
            }
            break;
        }
      } else {

        //var map = MapBuilder.Build(TmxImporter.Load("castle.tmx"));
        //map.RenderPreview("preview.png");

        //var hWriter = new StringWriter();
        //var sWriter = new StringWriter();
        //WriteCxx(hWriter, sWriter);
        //Console.WriteLine("Source:\n{0}", sWriter.ToString());

        var writer = new StringWriter();
        WriteLua(writer);
        Console.WriteLine(writer.ToString());

        //SoWhenIAskedHerOutSheSaidIWasntHerType("woods.tmx");

        //GameScript.LoadFromXML();
        //DialogDatabase.LoadFromXML();
      }
    }

    public static void WriteLua(TextWriter writer) {
      writer.WriteLine("-- GENERATED BY MAPTOOL.EXE, DO NOT EDIT BY HAND");

      writer.WriteLine("\n-- MAP IMAGES");

      foreach(var filePath in Directory.GetFiles(".", "*.tmx")) {
        var name = Path.GetFileNameWithoutExtension(filePath);
        writer.WriteLine(
          "TileSet_{0} = image{{ \"{0}.png\", width=16, height=16 }}", name
        );
        if (File.Exists(name + "_overlay.png")) {
          writer.WriteLine(
            "Overlay_{0} = image{{ \"{0}_overlay.png\", width=16, height=16 }}", name
          );
        }
        if (!File.Exists(name + "_blank.png")) {
          throw new Exception(string.Format("Could Not find Blank Image: {0}_blank.png", name));
        } else {
          writer.WriteLine(
            "Blank_{0} = image{{ \"{0}_blank.png\", width=128, height=128 }}", name
          );
        }
      }

      // images for dialogs
      var dd = DialogDatabase.LoadFromXML();

      writer.WriteLine("\n-- NPC IMAGES");
      foreach(string path in dd.ListNpcImagePaths()) {
        var name = path.Substring(0, path.Length - ".png".Length);
        writer.WriteLine("NPC_{0} = image{{ \"{0}.png\", width=32, height=32, pinned=true }}", name);
      }

      writer.WriteLine("\n-- DIALOG DETAIL IMAGES");
      foreach(string path in dd.ListDetailImagePaths()) {
        var name = path.Substring(0, path.Length - ".png".Length);
        writer.WriteLine("DETAIL_{0} = image{{ \"{0}.png\" }}", name);
      }
    }

    public static void WriteCxx(TextWriter hWriter, TextWriter sWriter) {
      string[] filePaths = Directory.GetFiles(".", "*.tmx");

      hWriter.WriteLine("// GENERATED BY MAPTOOL.EXE, DO NOT EDIT BY HAND");
      hWriter.WriteLine("#pragma once");
      hWriter.WriteLine("#include \"MapData.h\"");
      hWriter.WriteLine("");

      sWriter.WriteLine("// GENERATED BY MAPTOOL.EXE, DO NOT EDIT BY HAND");
      sWriter.WriteLine("#include \"gen_mapdata.h\"");
      sWriter.WriteLine("#include \"game.h\"");
      sWriter.WriteLine("");

      var database = new MapDatabase();

      foreach(string file in filePaths) {
        Console.WriteLine("Processing Map: " + file);
        var tmx = TmxImporter.Load(file);
        if (tmx == null) {
          throw new Exception("Could not load: " + file);
        }
        var map = MapBuilder.Build(tmx);
        database.RegisterMap(map);
      }
      foreach(var map in database.NameToMap.Values) {
        map.GenerateHeader(hWriter);
        map.GenerateSource(sWriter);
        sWriter.WriteLine("");
      }
      hWriter.WriteLine("");
    }

    static void SoWhenIAskedHerOutSheSaidIWasntHerType(string fileName) {
      Console.WriteLine("Reading TMX Data...");
      var tmxData = TmxImporter.Load(fileName);
      if (tmxData == null) { return; }
      tmxData.DrawBackgroundLayerMask();
      Console.WriteLine("");
      Console.WriteLine("Building Game Map...");
      var map = MapBuilder.Build(tmxData);
      if (map == null) { return; }
      map.DrawPortals();
      Console.WriteLine("");
      var textWriter = new StringWriter();
      map.GenerateSource(textWriter);
      Console.WriteLine(textWriter.ToString());
    }

  }



}

