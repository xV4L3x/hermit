// overflow_func.ts - funzione che triggera SmallFuncHeader overflow
// Generata da seeder/gen_overflow_sample.py
//
// environmentSize = 260 > 255  ->  overflowed = 1
// hasExceptionHandler = true  (try/catch/throw)

// prettier-ignore
function makeOverflow(
    p1: any, p2: any, p3: any, p4: any, p5: any, p6: any, p7: any
): any[] {

    // 260 variabili locali catturate dalle closures -> environmentSize = 260
    let v000 = p1 + 0;  let v001 = p1 + 1;  let v002 = p1 + 2;  let v003 = p1 + 3;  let v004 = p1 + 4;  let v005 = p1 + 5;  let v006 = p1 + 6;  let v007 = p1 + 7;
    let v008 = p1 + 8;  let v009 = p1 + 9;  let v010 = p1 + 10;  let v011 = p1 + 11;  let v012 = p1 + 12;  let v013 = p1 + 13;  let v014 = p1 + 14;  let v015 = p1 + 15;
    let v016 = p1 + 16;  let v017 = p1 + 17;  let v018 = p1 + 18;  let v019 = p1 + 19;  let v020 = p1 + 20;  let v021 = p1 + 21;  let v022 = p1 + 22;  let v023 = p1 + 23;
    let v024 = p1 + 24;  let v025 = p1 + 25;  let v026 = p1 + 26;  let v027 = p1 + 27;  let v028 = p1 + 28;  let v029 = p1 + 29;  let v030 = p1 + 30;  let v031 = p1 + 31;
    let v032 = p1 + 32;  let v033 = p1 + 33;  let v034 = p1 + 34;  let v035 = p1 + 35;  let v036 = p1 + 36;  let v037 = p1 + 37;  let v038 = p1 + 38;  let v039 = p1 + 39;
    let v040 = p1 + 40;  let v041 = p1 + 41;  let v042 = p1 + 42;  let v043 = p1 + 43;  let v044 = p1 + 44;  let v045 = p1 + 45;  let v046 = p1 + 46;  let v047 = p1 + 47;
    let v048 = p1 + 48;  let v049 = p1 + 49;  let v050 = p1 + 50;  let v051 = p1 + 51;  let v052 = p1 + 52;  let v053 = p1 + 53;  let v054 = p1 + 54;  let v055 = p1 + 55;
    let v056 = p1 + 56;  let v057 = p1 + 57;  let v058 = p1 + 58;  let v059 = p1 + 59;  let v060 = p1 + 60;  let v061 = p1 + 61;  let v062 = p1 + 62;  let v063 = p1 + 63;
    let v064 = p1 + 64;  let v065 = p1 + 65;  let v066 = p1 + 66;  let v067 = p1 + 67;  let v068 = p1 + 68;  let v069 = p1 + 69;  let v070 = p1 + 70;  let v071 = p1 + 71;
    let v072 = p1 + 72;  let v073 = p1 + 73;  let v074 = p1 + 74;  let v075 = p1 + 75;  let v076 = p1 + 76;  let v077 = p1 + 77;  let v078 = p1 + 78;  let v079 = p1 + 79;
    let v080 = p1 + 80;  let v081 = p1 + 81;  let v082 = p1 + 82;  let v083 = p1 + 83;  let v084 = p1 + 84;  let v085 = p1 + 85;  let v086 = p1 + 86;  let v087 = p1 + 87;
    let v088 = p1 + 88;  let v089 = p1 + 89;  let v090 = p1 + 90;  let v091 = p1 + 91;  let v092 = p1 + 92;  let v093 = p1 + 93;  let v094 = p1 + 94;  let v095 = p1 + 95;
    let v096 = p1 + 96;  let v097 = p1 + 97;  let v098 = p1 + 98;  let v099 = p1 + 99;  let v100 = p1 + 100;  let v101 = p1 + 101;  let v102 = p1 + 102;  let v103 = p1 + 103;
    let v104 = p1 + 104;  let v105 = p1 + 105;  let v106 = p1 + 106;  let v107 = p1 + 107;  let v108 = p1 + 108;  let v109 = p1 + 109;  let v110 = p1 + 110;  let v111 = p1 + 111;
    let v112 = p1 + 112;  let v113 = p1 + 113;  let v114 = p1 + 114;  let v115 = p1 + 115;  let v116 = p1 + 116;  let v117 = p1 + 117;  let v118 = p1 + 118;  let v119 = p1 + 119;
    let v120 = p1 + 120;  let v121 = p1 + 121;  let v122 = p1 + 122;  let v123 = p1 + 123;  let v124 = p1 + 124;  let v125 = p1 + 125;  let v126 = p1 + 126;  let v127 = p1 + 127;
    let v128 = p1 + 128;  let v129 = p1 + 129;  let v130 = p1 + 130;  let v131 = p1 + 131;  let v132 = p1 + 132;  let v133 = p1 + 133;  let v134 = p1 + 134;  let v135 = p1 + 135;
    let v136 = p1 + 136;  let v137 = p1 + 137;  let v138 = p1 + 138;  let v139 = p1 + 139;  let v140 = p1 + 140;  let v141 = p1 + 141;  let v142 = p1 + 142;  let v143 = p1 + 143;
    let v144 = p1 + 144;  let v145 = p1 + 145;  let v146 = p1 + 146;  let v147 = p1 + 147;  let v148 = p1 + 148;  let v149 = p1 + 149;  let v150 = p1 + 150;  let v151 = p1 + 151;
    let v152 = p1 + 152;  let v153 = p1 + 153;  let v154 = p1 + 154;  let v155 = p1 + 155;  let v156 = p1 + 156;  let v157 = p1 + 157;  let v158 = p1 + 158;  let v159 = p1 + 159;
    let v160 = p1 + 160;  let v161 = p1 + 161;  let v162 = p1 + 162;  let v163 = p1 + 163;  let v164 = p1 + 164;  let v165 = p1 + 165;  let v166 = p1 + 166;  let v167 = p1 + 167;
    let v168 = p1 + 168;  let v169 = p1 + 169;  let v170 = p1 + 170;  let v171 = p1 + 171;  let v172 = p1 + 172;  let v173 = p1 + 173;  let v174 = p1 + 174;  let v175 = p1 + 175;
    let v176 = p1 + 176;  let v177 = p1 + 177;  let v178 = p1 + 178;  let v179 = p1 + 179;  let v180 = p1 + 180;  let v181 = p1 + 181;  let v182 = p1 + 182;  let v183 = p1 + 183;
    let v184 = p1 + 184;  let v185 = p1 + 185;  let v186 = p1 + 186;  let v187 = p1 + 187;  let v188 = p1 + 188;  let v189 = p1 + 189;  let v190 = p1 + 190;  let v191 = p1 + 191;
    let v192 = p1 + 192;  let v193 = p1 + 193;  let v194 = p1 + 194;  let v195 = p1 + 195;  let v196 = p1 + 196;  let v197 = p1 + 197;  let v198 = p1 + 198;  let v199 = p1 + 199;
    let v200 = p1 + 200;  let v201 = p1 + 201;  let v202 = p1 + 202;  let v203 = p1 + 203;  let v204 = p1 + 204;  let v205 = p1 + 205;  let v206 = p1 + 206;  let v207 = p1 + 207;
    let v208 = p1 + 208;  let v209 = p1 + 209;  let v210 = p1 + 210;  let v211 = p1 + 211;  let v212 = p1 + 212;  let v213 = p1 + 213;  let v214 = p1 + 214;  let v215 = p1 + 215;
    let v216 = p1 + 216;  let v217 = p1 + 217;  let v218 = p1 + 218;  let v219 = p1 + 219;  let v220 = p1 + 220;  let v221 = p1 + 221;  let v222 = p1 + 222;  let v223 = p1 + 223;
    let v224 = p1 + 224;  let v225 = p1 + 225;  let v226 = p1 + 226;  let v227 = p1 + 227;  let v228 = p1 + 228;  let v229 = p1 + 229;  let v230 = p1 + 230;  let v231 = p1 + 231;
    let v232 = p1 + 232;  let v233 = p1 + 233;  let v234 = p1 + 234;  let v235 = p1 + 235;  let v236 = p1 + 236;  let v237 = p1 + 237;  let v238 = p1 + 238;  let v239 = p1 + 239;
    let v240 = p1 + 240;  let v241 = p1 + 241;  let v242 = p1 + 242;  let v243 = p1 + 243;  let v244 = p1 + 244;  let v245 = p1 + 245;  let v246 = p1 + 246;  let v247 = p1 + 247;
    let v248 = p1 + 248;  let v249 = p1 + 249;  let v250 = p1 + 250;  let v251 = p1 + 251;  let v252 = p1 + 252;  let v253 = p1 + 253;  let v254 = p1 + 254;  let v255 = p1 + 255;
    let v256 = p1 + 256;  let v257 = p1 + 257;  let v258 = p1 + 258;  let v259 = p1 + 259;

    // 260 closures che catturano le variabili
    const c000 = () => v000;  const c001 = () => v001;  const c002 = () => v002;  const c003 = () => v003;
    const c004 = () => v004;  const c005 = () => v005;  const c006 = () => v006;  const c007 = () => v007;
    const c008 = () => v008;  const c009 = () => v009;  const c010 = () => v010;  const c011 = () => v011;
    const c012 = () => v012;  const c013 = () => v013;  const c014 = () => v014;  const c015 = () => v015;
    const c016 = () => v016;  const c017 = () => v017;  const c018 = () => v018;  const c019 = () => v019;
    const c020 = () => v020;  const c021 = () => v021;  const c022 = () => v022;  const c023 = () => v023;
    const c024 = () => v024;  const c025 = () => v025;  const c026 = () => v026;  const c027 = () => v027;
    const c028 = () => v028;  const c029 = () => v029;  const c030 = () => v030;  const c031 = () => v031;
    const c032 = () => v032;  const c033 = () => v033;  const c034 = () => v034;  const c035 = () => v035;
    const c036 = () => v036;  const c037 = () => v037;  const c038 = () => v038;  const c039 = () => v039;
    const c040 = () => v040;  const c041 = () => v041;  const c042 = () => v042;  const c043 = () => v043;
    const c044 = () => v044;  const c045 = () => v045;  const c046 = () => v046;  const c047 = () => v047;
    const c048 = () => v048;  const c049 = () => v049;  const c050 = () => v050;  const c051 = () => v051;
    const c052 = () => v052;  const c053 = () => v053;  const c054 = () => v054;  const c055 = () => v055;
    const c056 = () => v056;  const c057 = () => v057;  const c058 = () => v058;  const c059 = () => v059;
    const c060 = () => v060;  const c061 = () => v061;  const c062 = () => v062;  const c063 = () => v063;
    const c064 = () => v064;  const c065 = () => v065;  const c066 = () => v066;  const c067 = () => v067;
    const c068 = () => v068;  const c069 = () => v069;  const c070 = () => v070;  const c071 = () => v071;
    const c072 = () => v072;  const c073 = () => v073;  const c074 = () => v074;  const c075 = () => v075;
    const c076 = () => v076;  const c077 = () => v077;  const c078 = () => v078;  const c079 = () => v079;
    const c080 = () => v080;  const c081 = () => v081;  const c082 = () => v082;  const c083 = () => v083;
    const c084 = () => v084;  const c085 = () => v085;  const c086 = () => v086;  const c087 = () => v087;
    const c088 = () => v088;  const c089 = () => v089;  const c090 = () => v090;  const c091 = () => v091;
    const c092 = () => v092;  const c093 = () => v093;  const c094 = () => v094;  const c095 = () => v095;
    const c096 = () => v096;  const c097 = () => v097;  const c098 = () => v098;  const c099 = () => v099;
    const c100 = () => v100;  const c101 = () => v101;  const c102 = () => v102;  const c103 = () => v103;
    const c104 = () => v104;  const c105 = () => v105;  const c106 = () => v106;  const c107 = () => v107;
    const c108 = () => v108;  const c109 = () => v109;  const c110 = () => v110;  const c111 = () => v111;
    const c112 = () => v112;  const c113 = () => v113;  const c114 = () => v114;  const c115 = () => v115;
    const c116 = () => v116;  const c117 = () => v117;  const c118 = () => v118;  const c119 = () => v119;
    const c120 = () => v120;  const c121 = () => v121;  const c122 = () => v122;  const c123 = () => v123;
    const c124 = () => v124;  const c125 = () => v125;  const c126 = () => v126;  const c127 = () => v127;
    const c128 = () => v128;  const c129 = () => v129;  const c130 = () => v130;  const c131 = () => v131;
    const c132 = () => v132;  const c133 = () => v133;  const c134 = () => v134;  const c135 = () => v135;
    const c136 = () => v136;  const c137 = () => v137;  const c138 = () => v138;  const c139 = () => v139;
    const c140 = () => v140;  const c141 = () => v141;  const c142 = () => v142;  const c143 = () => v143;
    const c144 = () => v144;  const c145 = () => v145;  const c146 = () => v146;  const c147 = () => v147;
    const c148 = () => v148;  const c149 = () => v149;  const c150 = () => v150;  const c151 = () => v151;
    const c152 = () => v152;  const c153 = () => v153;  const c154 = () => v154;  const c155 = () => v155;
    const c156 = () => v156;  const c157 = () => v157;  const c158 = () => v158;  const c159 = () => v159;
    const c160 = () => v160;  const c161 = () => v161;  const c162 = () => v162;  const c163 = () => v163;
    const c164 = () => v164;  const c165 = () => v165;  const c166 = () => v166;  const c167 = () => v167;
    const c168 = () => v168;  const c169 = () => v169;  const c170 = () => v170;  const c171 = () => v171;
    const c172 = () => v172;  const c173 = () => v173;  const c174 = () => v174;  const c175 = () => v175;
    const c176 = () => v176;  const c177 = () => v177;  const c178 = () => v178;  const c179 = () => v179;
    const c180 = () => v180;  const c181 = () => v181;  const c182 = () => v182;  const c183 = () => v183;
    const c184 = () => v184;  const c185 = () => v185;  const c186 = () => v186;  const c187 = () => v187;
    const c188 = () => v188;  const c189 = () => v189;  const c190 = () => v190;  const c191 = () => v191;
    const c192 = () => v192;  const c193 = () => v193;  const c194 = () => v194;  const c195 = () => v195;
    const c196 = () => v196;  const c197 = () => v197;  const c198 = () => v198;  const c199 = () => v199;
    const c200 = () => v200;  const c201 = () => v201;  const c202 = () => v202;  const c203 = () => v203;
    const c204 = () => v204;  const c205 = () => v205;  const c206 = () => v206;  const c207 = () => v207;
    const c208 = () => v208;  const c209 = () => v209;  const c210 = () => v210;  const c211 = () => v211;
    const c212 = () => v212;  const c213 = () => v213;  const c214 = () => v214;  const c215 = () => v215;
    const c216 = () => v216;  const c217 = () => v217;  const c218 = () => v218;  const c219 = () => v219;
    const c220 = () => v220;  const c221 = () => v221;  const c222 = () => v222;  const c223 = () => v223;
    const c224 = () => v224;  const c225 = () => v225;  const c226 = () => v226;  const c227 = () => v227;
    const c228 = () => v228;  const c229 = () => v229;  const c230 = () => v230;  const c231 = () => v231;
    const c232 = () => v232;  const c233 = () => v233;  const c234 = () => v234;  const c235 = () => v235;
    const c236 = () => v236;  const c237 = () => v237;  const c238 = () => v238;  const c239 = () => v239;
    const c240 = () => v240;  const c241 = () => v241;  const c242 = () => v242;  const c243 = () => v243;
    const c244 = () => v244;  const c245 = () => v245;  const c246 = () => v246;  const c247 = () => v247;
    const c248 = () => v248;  const c249 = () => v249;  const c250 = () => v250;  const c251 = () => v251;
    const c252 = () => v252;  const c253 = () => v253;  const c254 = () => v254;  const c255 = () => v255;
    const c256 = () => v256;  const c257 = () => v257;  const c258 = () => v258;  const c259 = () => v259;

    // try/catch/throw - imposta hasExceptionHandler = true
    try {
        if (typeof c000() !== 'number') throw new Error('type mismatch');
        if (typeof c001() !== 'number') throw new Error('type mismatch');
    } catch (e: any) {
        console.log('caught:', e.message);
        throw e;
    }

    return [
        c000, c001, c002, c003, c004, c005, c006, c007,
        c008, c009, c010, c011, c012, c013, c014, c015,
        c016, c017, c018, c019, c020, c021, c022, c023,
        c024, c025, c026, c027, c028, c029, c030, c031,
        c032, c033, c034, c035, c036, c037, c038, c039,
        c040, c041, c042, c043, c044, c045, c046, c047,
        c048, c049, c050, c051, c052, c053, c054, c055,
        c056, c057, c058, c059, c060, c061, c062, c063,
        c064, c065, c066, c067, c068, c069, c070, c071,
        c072, c073, c074, c075, c076, c077, c078, c079,
        c080, c081, c082, c083, c084, c085, c086, c087,
        c088, c089, c090, c091, c092, c093, c094, c095,
        c096, c097, c098, c099, c100, c101, c102, c103,
        c104, c105, c106, c107, c108, c109, c110, c111,
        c112, c113, c114, c115, c116, c117, c118, c119,
        c120, c121, c122, c123, c124, c125, c126, c127,
        c128, c129, c130, c131, c132, c133, c134, c135,
        c136, c137, c138, c139, c140, c141, c142, c143,
        c144, c145, c146, c147, c148, c149, c150, c151,
        c152, c153, c154, c155, c156, c157, c158, c159,
        c160, c161, c162, c163, c164, c165, c166, c167,
        c168, c169, c170, c171, c172, c173, c174, c175,
        c176, c177, c178, c179, c180, c181, c182, c183,
        c184, c185, c186, c187, c188, c189, c190, c191,
        c192, c193, c194, c195, c196, c197, c198, c199,
        c200, c201, c202, c203, c204, c205, c206, c207,
        c208, c209, c210, c211, c212, c213, c214, c215,
        c216, c217, c218, c219, c220, c221, c222, c223,
        c224, c225, c226, c227, c228, c229, c230, c231,
        c232, c233, c234, c235, c236, c237, c238, c239,
        c240, c241, c242, c243, c244, c245, c246, c247,
        c248, c249, c250, c251, c252, c253, c254, c255,
        c256, c257, c258, c259
    ];
}

const fns = makeOverflow(1, 2, 3, 4, 5, 6, 7);
console.log('closures:', fns.length);
